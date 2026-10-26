#include "render/renderer.hpp"

#include "core/log.hpp"
#include "render/render_target.hpp"
#include "render/shader_blobs.hpp"
#include "render/swapchain.hpp"
#include "render/view_math.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace apex::render {

namespace {

enum class Kind : std::uint32_t {
    Textured, Flat, WireFill, Edges, BoxSolid, BoxWire, Sprite, Grid,
    Outline, OutlineAlways, BoxOutline, BoxOutlineAlways, SpriteOutline, SpriteOutlineAlways,
    DepthOnly, BoxDepthOnly, Overlay, WorldOverlay, WorldOccluded, GhostTextured, GhostFlat, Label, Count
};
constexpr std::size_t kKindCount = static_cast<std::size_t>(Kind::Count);

// Relative depth pull per AlphaMode layer (opaque 0, alpha test 1, blend 2), so coplanar
// alpha-tested and translucent faces win against the opaque face under them. Applied as a
// clip-z scale like the grid's surface bias: dynamic depth-bias units are implementation-defined.
// Well above D32 resolution (~6e-8) and far below the grid's 2e-4.
constexpr float kLayerBias = 1e-5f;

enum class Input { Brush, Box, Sprite, None };

struct Spec {
    const char* vert;
    const char* frag;
    Input input;
    VkPrimitiveTopology topology;
    VkPolygonMode polygon;
    bool depthWrite;
    bool blend;
    bool depthBias;
    bool depthTest = true;
    bool colourWrite = true;
    VkCompareOp compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
};

constexpr Spec kSpecs[kKindCount] = {
    {"brush_vert", "brush_textured_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {"brush_vert", "brush_flat_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {"brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_LINE, true, false, true},
    {"brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, true, false, true},
    {"box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {"box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {"sprite_vert", "sprite_frag", Input::Sprite, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {"grid_vert", "grid_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, true},
    {"brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, true},
    {"brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, false, false},
    {"box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, true},
    {"box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, false, false},
    {"sprite_vert", "sprite_frag", Input::Sprite, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, true},
    {"sprite_vert", "sprite_frag", Input::Sprite, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, false, false},
    {"brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false, true, false},
    {"box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false, true, false},
    {"overlay_vert", "vcolor_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false, false},
    {"overlay_vert", "vcolor_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false},
    {"overlay_vert", "vcolor_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false, true, true, VK_COMPARE_OP_LESS},
    {"brush_vert", "brush_textured_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false},
    {"brush_vert", "brush_flat_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false},
    {"overlay_vert", "label_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false, false},
};

constexpr const char* kShaderIds[] = {"brush_vert", "brush_textured_frag", "brush_flat_frag", "solid_frag", "box_vert",
                                      "vcolor_frag", "sprite_vert", "sprite_frag", "grid_vert", "grid_frag", "overlay_vert", "label_frag"};

struct OverlayVertex {
    Vec4 position;
    Vec4 colour;
    Vec4 uv{0.0f};
};

// Expands the overlay primitives to clip-space triangles. Primitives behind the near plane are
// clipped in clip space (w = kNearW), so a gizmo straddling the camera plane stays sane.
std::vector<OverlayVertex> expandOverlay(const DrawList& list, std::span<const OverlaySegment> segments,
                                          std::span<const OverlayTriangle> triangles,
                                          std::span<const OverlayPoint> points = {}) {
    constexpr float kNearW = 1e-3f;
    std::vector<OverlayVertex> out;
    const Vec2 size = list.viewportSize;
    if (size.x <= 0.0f || size.y <= 0.0f) return out;
    const auto clip = [&](DVec3 world) {
        return list.view.viewProjection * Vec4(toCameraRelative(world, list.view.cameraPosition), 1.0f);
    };
    const auto lerp = [](Vec4 a, Vec4 b, float t) { return a + (b - a) * t; };
    for (const OverlaySegment& seg : segments) {
        Vec4 a = clip(seg.a), b = clip(seg.b);
        if (a.w < kNearW && b.w < kNearW) continue;
        if (a.w < kNearW) a = lerp(a, b, (kNearW - a.w) / (b.w - a.w));
        else if (b.w < kNearW) b = lerp(b, a, (kNearW - b.w) / (a.w - b.w));
        const Vec2 pa = Vec2(a.x, a.y) / a.w * size * 0.5f, pb = Vec2(b.x, b.y) / b.w * size * 0.5f;
        Vec2 d = pb - pa;
        const float length = glm::length(d);
        d = length > 1e-6f ? d / length : Vec2(1.0f, 0.0f);
        const Vec2 n = Vec2(-d.y, d.x) * (seg.widthPixels * 0.5f);
        const auto vertex = [&](Vec4 c, Vec2 p, Vec2 offset) {
            const Vec2 ndc = (p + offset) * 2.0f / size;
            return OverlayVertex{Vec4(ndc * c.w, c.z, c.w), seg.colour};
        };
        const OverlayVertex q[4] = {vertex(a, pa, n), vertex(a, pa, -n), vertex(b, pb, -n), vertex(b, pb, n)};
        for (const int i : {0, 1, 2, 0, 2, 3}) out.push_back(q[i]);
    }
    for (const OverlayTriangle& tri : triangles) {
        Vec4 v[3] = {clip(tri.a), clip(tri.b), clip(tri.c)};
        // Skip anything touching the near plane; filled handles are small and in front of the camera.
        if (v[0].w < kNearW || v[1].w < kNearW || v[2].w < kNearW) continue;
        for (const Vec4& c : v) out.push_back({c, tri.colour});
    }
    for (const auto& point : points) {
        const Vec4 c = clip(point.position);
        if (c.w < kNearW) continue;
        const Vec2 half = Vec2(point.sizePixels) / size * c.w;
        const OverlayVertex q[] = {
            {c + Vec4(-half.x, -half.y, 0, 0), point.colour},
            {c + Vec4( half.x, -half.y, 0, 0), point.colour},
            {c + Vec4( half.x,  half.y, 0, 0), point.colour},
            {c + Vec4(-half.x,  half.y, 0, 0), point.colour}};
        for (const int i : {0, 1, 2, 0, 2, 3}) out.push_back(q[i]);
    }
    return out;
}

std::vector<OverlayVertex> expandLabels(const DrawList& list, const GlyphAtlasData& atlas) {
    std::vector<OverlayVertex> out;
    if (list.viewportSize.x <= 0 || list.viewportSize.y <= 0) return out;
    for (const auto& label : list.labels) {
        const Vec4 anchor = list.view.viewProjection * Vec4(toCameraRelative(label.position, list.view.cameraPosition), 1);
        if (anchor.w < 1e-3f || anchor.z < 0 || anchor.z > anchor.w) continue;
        Vec2 pen = label.offset;
        for (std::size_t i = 0; i < label.text.size();) {
            const auto first = static_cast<unsigned char>(label.text[i++]);
            unsigned code = first;
            if (first >= 0x80) {
                unsigned count = first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : first >= 0xC2 ? 1 : 0;
                code = count ? first & ((1u << (6 - count)) - 1) : '?';
                bool valid = count != 0;
                for (unsigned j = 0; j < count; ++j) {
                    if (i >= label.text.size() || (static_cast<unsigned char>(label.text[i]) & 0xC0) != 0x80) {
                        valid = false;
                        break;
                    }
                    code = (code << 6) | (static_cast<unsigned char>(label.text[i++]) & 0x3F);
                }
                // Reject overlong forms: each length has a smallest code point it may encode.
                constexpr unsigned minimum[] = {0, 0x80, 0x800, 0x10000};
                if (!valid || code < minimum[count] || code > 255) code = '?';
            }
            if (code == '\n') { pen.x = label.offset.x; pen.y += atlas.lineHeight * list.labelScale; continue; }
            const auto& glyph = atlas.glyphs[code];
            const auto& r = glyph.rect;
            const Vec2 lo = pen + glyph.bearing * list.labelScale;
            const Vec2 hi = lo + Vec2(r[2], r[3]) * list.labelScale;
            const Vec2 uvLo = Vec2(r[0], r[1]) / Vec2(atlas.width, atlas.height);
            const Vec2 uvHi = Vec2(r[0] + r[2], r[1] + r[3]) / Vec2(atlas.width, atlas.height);
            const auto vertex = [&](Vec2 pixel, Vec2 uv) {
                return OverlayVertex{anchor + Vec4(pixel * 2.0f / list.viewportSize * anchor.w, 0, 0), label.colour, Vec4(uv, 0, 0)};
            };
            if (r[2] && r[3]) {
                const OverlayVertex q[] = {vertex(lo, uvLo), vertex({hi.x, lo.y}, {uvHi.x, uvLo.y}),
                                           vertex(hi, uvHi), vertex({lo.x, hi.y}, {uvLo.x, uvHi.y})};
                for (const int index : {0, 1, 2, 0, 2, 3}) out.push_back(q[index]);
            }
            pen.x += glyph.advance * list.labelScale;
        }
    }
    return out;
}

PushConstants basePush(const DrawList& list) {
    PushConstants push{};
    push.viewProjection = list.view.viewProjection;
    push.colour = Vec4(1.0f);
    return push;
}

Vec4 slotVec(Vec3 offset, std::uint32_t slot) {
    return Vec4(offset, std::bit_cast<float>(slot));
}

} // namespace

struct Renderer::Impl {
    Context* context = nullptr;
    TextureHeap* heap = nullptr;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipelines[kKindCount]{};
    std::map<std::string, std::vector<std::uint32_t>> overrides; // Hot-reloaded SPIR-V.
    // Swapchains have one frame in flight and distinct command buffers. A slot is reused only
    // after its fence, so host writes cannot race another viewport or an earlier submission.
    std::map<VkCommandBuffer, Buffer> previewBuffers;
    std::shared_ptr<const GlyphAtlas> atlas;
    std::map<VkCommandBuffer, Buffer> overlayBuffers; // Same slot discipline as previewBuffers.
#ifdef APEX_SHADER_DIR
    std::map<std::string, std::filesystem::file_time_type> stamps;
#endif

    ~Impl() {
        if (!previewBuffers.empty() || !overlayBuffers.empty()) context->waitIdle();
        destroyPipelines();
        if (layout) vkDestroyPipelineLayout(context->device(), layout, nullptr);
    }

    void destroyPipelines() {
        for (VkPipeline& pipeline : pipelines) {
            if (pipeline) vkDestroyPipeline(context->device(), pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }

    Result<VkShaderModule> module(const std::string& id) const {
        std::span<const std::uint32_t> code = builtinSpirv(id);
        if (auto it = overrides.find(id); it != overrides.end()) code = it->second;
        if (code.empty()) return std::unexpected(Error{ErrorCode::NotFound, "no SPIR-V for shader " + id});
        VkShaderModuleCreateInfo info = vkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        info.codeSize = code.size_bytes();
        info.pCode = code.data();
        VkShaderModule result = VK_NULL_HANDLE;
        APEX_VK_TRY(vkCreateShaderModule(context->device(), &info, nullptr, &result), "vkCreateShaderModule " + id);
        return result;
    }

    // Builds one pipeline over dynamic rendering. Returns VK_NULL_HANDLE (not an error) for kinds
    // the device cannot do.
    Result<VkPipeline> build(Kind kind) const {
        const Spec& spec = kSpecs[static_cast<std::size_t>(kind)];
        if (spec.polygon == VK_POLYGON_MODE_LINE && !context->info().fillModeNonSolid) return VkPipeline(VK_NULL_HANDLE);

        auto vert = module(spec.vert);
        if (!vert) return std::unexpected(vert.error());
        auto frag = module(spec.frag);
        if (!frag) {
            vkDestroyShaderModule(context->device(), *vert, nullptr);
            return std::unexpected(frag.error());
        }
        struct ModuleGuard {
            VkDevice device;
            VkShaderModule a, b;
            ~ModuleGuard() {
                vkDestroyShaderModule(device, a, nullptr);
                vkDestroyShaderModule(device, b, nullptr);
            }
        } guard{context->device(), *vert, *frag};

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = vkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = *vert;
        stages[0].pName = "main";
        stages[1] = stages[0];
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = *frag;

        VkVertexInputBindingDescription binding{0, 0, VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attributes;
        switch (spec.input) {
        case Input::Brush:
            binding.stride = sizeof(BrushVertex);
            attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BrushVertex, position)},
                          {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BrushVertex, normal)},
                          {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(BrushVertex, uv)},
                          {3, 0, VK_FORMAT_R32_UINT, offsetof(BrushVertex, material)}};
            break;
        case Input::Box:
            binding.stride = sizeof(BoxInstance);
            binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
            attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BoxInstance, mins)},
                          {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BoxInstance, maxs)},
                          {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(BoxInstance, colour)},
                          {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BoxInstance, origin)},
                          {4, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BoxInstance, basis)},
                          {5, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BoxInstance, basis) + 3 * sizeof(float)},
                          {6, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BoxInstance, basis) + 6 * sizeof(float)}};
            break;
        case Input::Sprite:
            binding.stride = sizeof(SpriteInstance);
            binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
            attributes = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstance, position)},
                          {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstance, colour)},
                          {2, 0, VK_FORMAT_R32_UINT, offsetof(SpriteInstance, texture)}};
            break;
        case Input::None: break;
        }
        VkPipelineVertexInputStateCreateInfo vertexInput = vkStruct<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        if (spec.input != Input::None) {
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
            vertexInput.pVertexAttributeDescriptions = attributes.data();
        }
        VkPipelineInputAssemblyStateCreateInfo assembly = vkStruct<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        assembly.topology = spec.topology;
        VkPipelineViewportStateCreateInfo viewport = vkStruct<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster = vkStruct<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        raster.polygonMode = spec.polygon;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.depthBiasEnable = spec.depthBias ? VK_TRUE : VK_FALSE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample = vkStruct<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth = vkStruct<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depth.depthTestEnable = spec.depthTest ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = spec.depthWrite ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = spec.compare; // Reversed-Z.
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (!spec.colourWrite) attachment.colorWriteMask = 0;
        if (spec.blend) {
            attachment.blendEnable = VK_TRUE;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo blend = vkStruct<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
        VkPipelineDynamicStateCreateInfo dynamic = vkStruct<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount = 3;
        dynamic.pDynamicStates = dynamicStates;
        VkPipelineRenderingCreateInfo rendering = vkStruct<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat = depthFormat;

        VkGraphicsPipelineCreateInfo info = vkStruct<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        APEX_VK_TRY(vkCreateGraphicsPipelines(context->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
                    "vkCreateGraphicsPipelines");
        return pipeline;
    }

    Result<void> buildAll() {
        VkPipeline built[kKindCount]{};
        for (std::size_t i = 0; i < kKindCount; ++i) {
            auto pipeline = build(static_cast<Kind>(i));
            if (!pipeline) {
                for (VkPipeline p : built) {
                    if (p) vkDestroyPipeline(context->device(), p, nullptr);
                }
                return std::unexpected(pipeline.error());
            }
            built[i] = *pipeline;
        }
        destroyPipelines();
        std::copy(std::begin(built), std::end(built), pipelines);
        return {};
    }

    void bind(VkCommandBuffer commands, Kind kind) const {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[static_cast<std::size_t>(kind)]);
    }
    void pushAll(VkCommandBuffer commands, const PushConstants& push) const {
        vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_ALL, 0, sizeof(PushConstants), &push);
    }
    bool has(Kind kind) const { return pipelines[static_cast<std::size_t>(kind)] != VK_NULL_HANDLE; }
};

Renderer::Renderer(std::unique_ptr<Impl> impl, bool wireFill) : impl_(std::move(impl)), wireFill_(wireFill) {}
Renderer::~Renderer() = default;
void Renderer::setGlyphAtlas(std::shared_ptr<const GlyphAtlas> atlas) {
    if (impl_->atlas == atlas) return;
    impl_->context->waitIdle();
    impl_->atlas = std::move(atlas);
}

Result<std::unique_ptr<Renderer>> Renderer::create(Context& context, TextureHeap& heap, VkFormat colorFormat,
                                                   VkFormat depthFormat) {
    auto impl = std::make_unique<Impl>();
    impl->context = &context;
    impl->heap = &heap;
    impl->colorFormat = colorFormat;
    impl->depthFormat = depthFormat;

    const VkPushConstantRange range{VK_SHADER_STAGE_ALL, 0, sizeof(PushConstants)};
    const VkDescriptorSetLayout setLayout = heap.layout();
    VkPipelineLayoutCreateInfo layoutInfo = vkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    APEX_VK_TRY(vkCreatePipelineLayout(context.device(), &layoutInfo, nullptr, &impl->layout), "vkCreatePipelineLayout");

#ifdef APEX_SHADER_DIR
    for (const char* id : kShaderIds) {
        std::string file = id;
        file[file.rfind('_')] = '.';
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(std::filesystem::path(APEX_SHADER_DIR) / (file + ".glsl"), ec);
        if (!ec) impl->stamps[id] = stamp;
    }
#endif
    if (auto built = impl->buildAll(); !built) return std::unexpected(built.error().withContext("build pipelines"));
    const bool wire = context.info().fillModeNonSolid;
    return std::unique_ptr<Renderer>(new Renderer(std::move(impl), wire));
}

Result<bool> Renderer::reloadChangedShaders() {
#ifdef APEX_SHADER_DIR
    namespace fs = std::filesystem;
    std::vector<std::string> changed;
    for (const char* id : kShaderIds) {
        std::string file = id;
        file[file.rfind('_')] = '.';
        std::error_code ec;
        const auto stamp = fs::last_write_time(fs::path(APEX_SHADER_DIR) / (file + ".glsl"), ec);
        if (!ec && stamp != impl_->stamps[id]) changed.push_back(id);
    }
    // The .inc files are shared by every shader: any change recompiles everything.
    for (const auto& entry : fs::directory_iterator(APEX_SHADER_DIR)) {
        if (entry.path().extension() != ".inc") continue;
        static std::map<std::string, fs::file_time_type> incStamps;
        auto& seen = incStamps[entry.path().string()];
        const auto stamp = entry.last_write_time();
        if (seen != fs::file_time_type{} && seen != stamp) {
            changed.clear();
            for (const char* id : kShaderIds) changed.push_back(id);
        }
        seen = stamp;
    }
    if (changed.empty()) return false;

    const fs::path tmp = fs::temp_directory_path() / "apex_shader_reload";
    fs::create_directories(tmp);
    auto overrides = impl_->overrides;
    for (const std::string& id : changed) {
        std::string file = id;
        file[file.rfind('_')] = '.';
        const fs::path source = fs::path(APEX_SHADER_DIR) / (file + ".glsl");
        const fs::path out = tmp / (id + ".spv");
        const std::string stage = file.substr(file.rfind('.') + 1);
        const std::string command = "glslc -fshader-stage=" + stage + " --target-env=vulkan1.2 -I \"" + APEX_SHADER_DIR +
                                    "\" -o \"" + out.string() + "\" \"" + source.string() + "\"";
        std::error_code ec;
        impl_->stamps[id] = fs::last_write_time(source, ec);
        if (std::system(command.c_str()) != 0) {
            log::warn("shader reload: {} failed to compile; keeping previous version", file);
            return false;
        }
        std::ifstream in(out, std::ios::binary | std::ios::ate);
        const auto size = static_cast<std::size_t>(in.tellg());
        std::vector<std::uint32_t> words(size / 4);
        in.seekg(0);
        in.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
        overrides[id] = std::move(words);
    }
    impl_->context->waitIdle();
    std::swap(impl_->overrides, overrides);
    if (auto built = impl_->buildAll(); !built) {
        std::swap(impl_->overrides, overrides); // Keep the working set.
        log::warn("shader reload: pipeline rebuild failed: {}", built.error().what());
        return false;
    }
    log::info("shader reload: rebuilt {} shader(s)", changed.size());
    return true;
#else
    return false;
#endif
}

void Renderer::record(VkCommandBuffer commands, const DrawList& list) {
    Impl& s = *impl_;
    VkDeviceAddress previewAddress = 0;
    if (!list.previews.empty()) {
        const VkDeviceSize size = list.previews.size() * sizeof(PreviewTransform);
        Buffer& buffer = s.previewBuffers[commands];
        if (buffer.size() < size) {
            auto created = Buffer::create(*s.context, {std::max(size, buffer.size() * 2),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
            if (!created) {
                log::error("preview transform buffer: {}", created.error().what());
                return;
            }
            buffer = std::move(*created);
        }
        auto* packed = static_cast<PreviewTransform*>(buffer.mapped());
        for (std::size_t i = 0; i < list.previews.size(); ++i) {
            const auto& preview = list.previews[i];
            packed[i] = cameraRelativePreview(preview.matrix, preview.origin, list.view.cameraPosition);
        }
        buffer.flush(0, size);
        previewAddress = buffer.deviceAddress();
    }
    const auto previewPush = [&](PushConstants& push, std::uint32_t index, bool sprite = false, bool outline = false) {
        assert(index <= list.previews.size());
        const float low = std::bit_cast<float>(static_cast<std::uint32_t>(previewAddress));
        const float high = std::bit_cast<float>(static_cast<std::uint32_t>(previewAddress >> 32));
        if (sprite) {
            assert(index <= 0x7FFFFFFFu);
            push.offsetSlot.w = low;
            push.p1.w = high;
            push.p0.w = std::bit_cast<float>((index << 1) | (outline ? 1u : 0u));
        } else {
            push.p1 = Vec4(low, high, std::bit_cast<float>(index), 0.0f);
        }
    };
    const VkDescriptorSet set = s.heap->descriptorSet();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &set, 0, nullptr);
    vkCmdSetDepthBias(commands, 0.0f, 0.0f, 0.0f);
    const DVec3 camera = list.view.cameraPosition;

    auto drawMeshes = [&](Kind kind, const auto& meshes, auto&& setup, auto&& count, auto&& indices, bool edges) {
        if (!s.has(kind)) return;
        s.bind(commands, kind);
        for (const MeshDraw& mesh : meshes) {
            const std::uint32_t n = count(mesh);
            if (!mesh.vertices || !indices(mesh) || n == 0) continue;
            const VkDeviceSize zero = 0;
            VkBuffer vb = mesh.vertices->handle();
            vkCmdBindVertexBuffers(commands, 0, 1, &vb, &zero);
            vkCmdBindIndexBuffer(commands, indices(mesh)->handle(), 0, VK_INDEX_TYPE_UINT32);
            PushConstants push = basePush(list);
            push.offsetSlot = slotVec(toCameraRelative(mesh.origin, camera), mesh.texture);
            setup(push, mesh);
            previewPush(push, mesh.previewIndex);
            push.p1.w = std::bit_cast<float>(static_cast<std::uint32_t>(mesh.alpha));
            // Every pass of a mesh shares its layer's bias so its edges and outlines stay on its faces.
            const float layerScale = 1.0f + kLayerBias * static_cast<float>(mesh.alpha);
            for (int c = 0; c < 4; ++c) push.viewProjection[c][2] *= layerScale;
            s.pushAll(commands, push);
            vkCmdDrawIndexed(commands, n, 1, edges ? mesh.firstEdgeIndex : mesh.firstIndex, 0, 0);
        }
    };
    auto trianglesOf = [](const MeshDraw& m) { return m.indexCount; };
    auto trianglesBuf = [](const MeshDraw& m) { return m.indices; };
    auto edgesOf = [](const MeshDraw& m) { return m.edgeIndexCount; };
    auto edgesBuf = [](const MeshDraw& m) { return m.edgeIndices; };
    auto brushColour = [](PushConstants& p, const MeshDraw& m) {
        p.colour = m.colour;
        p.p0 = m.highlight;
    };

    switch (list.mode) {
    case DisplayMode::Textured: {
        const auto opaqueOf = [](const MeshDraw& m) { return m.alpha == AlphaMode::Blend ? 0u : m.indexCount; };
        drawMeshes(Kind::Textured, list.meshes, brushColour, opaqueOf, trianglesBuf, false);
        // Translucent solids last, farthest first (by draw origin), blended over the opaque scene.
        std::vector<MeshDraw> blended;
        for (const MeshDraw& mesh : list.meshes) {
            if (mesh.alpha == AlphaMode::Blend) blended.push_back(mesh);
        }
        const auto distance = [&](const MeshDraw& m) { return glm::dot(m.origin - camera, m.origin - camera); };
        std::ranges::stable_sort(blended, [&](const MeshDraw& a, const MeshDraw& b) { return distance(a) > distance(b); });
        drawMeshes(Kind::GhostTextured, blended, brushColour, trianglesOf, trianglesBuf, false);
        break;
    }
    case DisplayMode::Flat:
        drawMeshes(Kind::Flat, list.meshes, brushColour, trianglesOf, trianglesBuf, false);
        break;
    case DisplayMode::Wireframe: {
        // Prefer the real edge list; fall back to polygon-mode lines (shows triangulation).
        bool anyEdges = std::ranges::any_of(list.meshes, [](const MeshDraw& m) { return m.edgeIndices && m.edgeIndexCount; });
        if (anyEdges) {
            drawMeshes(Kind::Edges, list.meshes, brushColour,
                       [](const MeshDraw& m) { return m.outlineOnlyWire ? 0u : m.edgeIndexCount; }, edgesBuf, true);
        } else {
            drawMeshes(Kind::WireFill, list.meshes, brushColour,
                       [](const MeshDraw& m) { return m.outlineOnlyWire ? 0u : m.indexCount; }, trianglesBuf, false);
        }
        break;
    }
    }
    if (list.mode != DisplayMode::Wireframe)
        drawMeshes(list.mode == DisplayMode::Textured ? Kind::GhostTextured : Kind::GhostFlat,
                   list.ghosts, brushColour, trianglesOf, trianglesBuf, false);
    if (list.edgeOverlay && list.mode != DisplayMode::Wireframe && s.has(Kind::Edges)) {
        vkCmdSetDepthBias(commands, list.edgeDepthBias, 0.0f, 1.0f);
        drawMeshes(Kind::Edges, list.meshes, [&](PushConstants& p, const MeshDraw&) { p.colour = list.edgeColour; }, edgesOf, edgesBuf, true);
        vkCmdSetDepthBias(commands, 0.0f, 0.0f, 0.0f);
    }

    for (const BoxDraw& box : list.boxes) {
        if (!box.instances || box.count == 0 || (!box.solid && box.outlineOnlyWire)) continue;
        const Kind kind = box.solid ? Kind::BoxSolid : Kind::BoxWire;
        s.bind(commands, kind);
        const VkDeviceSize zero = 0;
        VkBuffer vb = box.instances->handle();
        vkCmdBindVertexBuffers(commands, 0, 1, &vb, &zero);
        PushConstants push = basePush(list);
        push.offsetSlot = slotVec(toCameraRelative(box.origin, camera), 0);
        push.p0 = Vec4(box.solid ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        push.colour = box.tint;
        previewPush(push, box.previewIndex);
        s.pushAll(commands, push);
        vkCmdDraw(commands, box.solid ? 36 : 24, box.count, 0, box.firstInstance);
    }

    if (!list.sprites.empty()) {
        s.bind(commands, Kind::Sprite);
        for (const SpriteDraw& sprite : list.sprites) {
            if (!sprite.instances || sprite.count == 0) continue;
            const VkDeviceSize zero = 0;
            VkBuffer vb = sprite.instances->handle();
            vkCmdBindVertexBuffers(commands, 0, 1, &vb, &zero);
            PushConstants push = basePush(list);
            push.offsetSlot = slotVec(toCameraRelative(sprite.origin, camera), 0);
            push.colour = sprite.tint;
            push.p0 = Vec4(list.view.cameraRight, 0.0f);
            push.p1 = Vec4(list.view.cameraUp, 0.0f);
            previewPush(push, sprite.previewIndex, true);
            s.pushAll(commands, push);
            vkCmdDraw(commands, 6, sprite.count, 0, sprite.firstInstance);
        }
    }

    if (list.grid.enabled) {
        s.bind(commands, Kind::Grid);
        PushConstants push = basePush(list);
        const GridSettings& grid = list.grid;
        const DVec2 projected = gridCameraCoordinates(camera, grid.origin, grid.uAxis, grid.vAxis);
        const Vec2 wrapped = wrapGridCoordinates(projected, grid.spacing);
        const Vec2 major = wrapGridCoordinates(projected, grid.spacing * 8.0);
        const double height = glm::dot(camera - grid.origin, grid.normal);
        // Vertices are relative to the camera's plane projection; fold the basis into the matrix.
        Mat4 planeToCamera(1.0f);
        planeToCamera[0] = Vec4(Vec3(grid.uAxis), 0.0f);
        planeToCamera[1] = Vec4(Vec3(grid.vAxis), 0.0f);
        planeToCamera[2] = Vec4(Vec3(grid.normal), 0.0f);
        planeToCamera[3] = Vec4(Vec3(-grid.normal * height), 1.0f);
        push.viewProjection *= planeToCamera;
        if (grid.bounds) {
            // Surface-anchored grid: pull depth toward the camera by a relative amount so it wins
            // against the coplanar face on every device. Dynamic depth bias units are
            // implementation-defined and left holes on RADV. Reversed Z: larger is closer.
            constexpr float kSurfaceBias = 2e-4f;
            for (int c = 0; c < 4; ++c) push.viewProjection[c][2] *= 1.0f + kSurfaceBias;
        }
        push.colour = grid.colour;
        push.p0 = Vec4(wrapped, major.x, grid.spacing);
        const double extent = grid.fadeDistance * 2.0;
        DVec2 lo(-extent), hi(extent), fadeOrigin(0.0);
        if (grid.bounds) {
            lo = DVec2(grid.bounds->uMin - grid.margin, grid.bounds->vMin - grid.margin) - projected;
            hi = DVec2(grid.bounds->uMax + grid.margin, grid.bounds->vMax + grid.margin) - projected;
            // Fade from the nearest point on the expanded rect to the projected camera. This
            // keeps an off-centre face visible; the far side still fades over fadeDistance.
            fadeOrigin = glm::clamp(DVec2(0.0), lo, hi);
        }
        push.p1 = Vec4(static_cast<float>(lo.x), static_cast<float>(hi.x),
                       static_cast<float>(lo.y), static_cast<float>(hi.y));
        push.offsetSlot = Vec4(Vec2(fadeOrigin), grid.fadeDistance, major.y);
        s.pushAll(commands, push);
        vkCmdDraw(commands, 64 * 64 * 6, 1, 0, 0);
    }
    // Wireframe keeps its normal hollow appearance, but outlines use surface occlusion.
    if (list.mode == DisplayMode::Wireframe &&
        (!list.outlines.empty() || !list.boxOutlines.empty() || !list.spriteOutlines.empty() ||
         !list.worldSegments.empty() || !list.worldPoints.empty() || !list.worldTriangles.empty())) {
        drawMeshes(Kind::DepthOnly, list.meshes, [](PushConstants&, const MeshDraw&) {},
                   trianglesOf, trianglesBuf, false);
        for (const BoxDraw& box : list.boxes) {
            if (!box.instances || box.count == 0) continue;
            s.bind(commands, Kind::BoxDepthOnly);
            const VkDeviceSize zero = 0;
            const VkBuffer vb = box.instances->handle();
            vkCmdBindVertexBuffers(commands, 0, 1, &vb, &zero);
            PushConstants push = basePush(list);
            push.offsetSlot = slotVec(toCameraRelative(box.origin, camera), 0);
            push.p0.x = 1.0f;
            previewPush(push, box.previewIndex);
            s.pushAll(commands, push);
            vkCmdDraw(commands, 36, box.count, 0, box.firstInstance);
        }
    }
    // Depth-disabled dim pass first; visible lines replace it at full strength. Keep depth intact.
    for (const bool visible : {false, true}) {
        vkCmdSetDepthBias(commands, visible ? list.edgeDepthBias : 0.0f, 0.0f, visible ? 1.0f : 0.0f);
        const float alpha = visible ? 1.0f : list.occludedAlpha;
        drawMeshes(visible ? Kind::Outline : Kind::OutlineAlways, list.outlines,
                   [&](PushConstants& p, const MeshDraw& m) {
                       p.colour = m.colour;
                       p.colour.a *= alpha;
                   }, edgesOf, edgesBuf, true);
        for (const BoxDraw& box : list.boxOutlines) {
            if (!box.instances || box.count == 0) continue;
            s.bind(commands, visible ? Kind::BoxOutline : Kind::BoxOutlineAlways);
            const VkDeviceSize zero = 0;
            const VkBuffer vb = box.instances->handle();
            vkCmdBindVertexBuffers(commands, 0, 1, &vb, &zero);
            PushConstants push = basePush(list);
            push.offsetSlot = slotVec(toCameraRelative(box.origin, camera), 0);
            push.p0.y = 1.0f;
            push.colour = box.tint;
            push.colour.a *= alpha;
            previewPush(push, box.previewIndex);
            s.pushAll(commands, push);
            vkCmdDraw(commands, 24, box.count, 0, box.firstInstance);
        }
        for (const SpriteDraw& sprite : list.spriteOutlines) {
            if (!sprite.instances || sprite.count == 0) continue;
            s.bind(commands, visible ? Kind::SpriteOutline : Kind::SpriteOutlineAlways);
            const VkDeviceSize zero = 0;
            const VkBuffer vb = sprite.instances->handle();
            vkCmdBindVertexBuffers(commands, 0, 1, &vb, &zero);
            PushConstants push = basePush(list);
            push.offsetSlot = slotVec(toCameraRelative(sprite.origin, camera), 0);
            push.p0 = Vec4(list.view.cameraRight, 1.0f);
            push.p1 = Vec4(list.view.cameraUp, 1.0f);
            push.colour = sprite.tint;
            push.colour.a *= alpha;
            previewPush(push, sprite.previewIndex, true, true);
            s.pushAll(commands, push);
            vkCmdDraw(commands, 8, sprite.count, 0, sprite.firstInstance);
        }
    }
    vkCmdSetDepthBias(commands, 0.0f, 0.0f, 0.0f);

    auto vertices = expandOverlay(list, list.worldSegments, list.worldTriangles, list.worldPoints);
    const auto worldCount = static_cast<std::uint32_t>(vertices.size());
    auto screen = expandOverlay(list, list.overlaySegments, list.overlayTriangles);
    const auto screenCount = static_cast<std::uint32_t>(screen.size());
    vertices.insert(vertices.end(), screen.begin(), screen.end());
    if (s.atlas) {
        auto labels = expandLabels(list, s.atlas->data());
        vertices.insert(vertices.end(), labels.begin(), labels.end());
    }
    if (!vertices.empty()) {
        const VkDeviceSize size = vertices.size() * sizeof(OverlayVertex);
        Buffer& buffer = s.overlayBuffers[commands];
        if (buffer.size() < size) {
            auto created = Buffer::create(*s.context, {std::max(size, buffer.size() * 2),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
            if (!created) {
                log::error("overlay buffer: {}", created.error().what());
                return;
            }
            buffer = std::move(*created);
        }
        std::memcpy(buffer.mapped(), vertices.data(), size);
        buffer.flush(0, size);
        const VkDeviceAddress address = buffer.deviceAddress();
        PushConstants push = basePush(list);
        push.p1 = Vec4(std::bit_cast<float>(static_cast<std::uint32_t>(address)),
                       std::bit_cast<float>(static_cast<std::uint32_t>(address >> 32)), 0.0f, 0.0f);
        const auto draw = [&](Kind kind, std::uint32_t count, std::uint32_t first, float alpha) {
            if (!count) return;
            push.colour.a = alpha;
            s.bind(commands, kind);
            s.pushAll(commands, push);
            vkCmdDraw(commands, count, 1, first, 0);
        };
        // Disjoint depth comparisons preserve the requested alpha of translucent triangles.
        draw(Kind::WorldOccluded, worldCount, 0, list.occludedAlpha);
        draw(Kind::WorldOverlay, worldCount, 0, 1.0f);
        draw(Kind::Overlay, screenCount, worldCount, 1.0f);
        if (s.atlas) {
            push.offsetSlot.w = std::bit_cast<float>(s.atlas->slot());
            draw(Kind::Label, static_cast<std::uint32_t>(vertices.size()) - worldCount - screenCount,
                 worldCount + screenCount, 1.0f);
        }
    }
}

Result<void> SwapchainDepth::begin(VkCommandBuffer commands, const SwapchainFrame& frame, const float clearColor[4]) {
    if (!depth_ || depth_.extent().width != frame.extent.width || depth_.extent().height != frame.extent.height) {
        // Swapchain::renderFrame waits for this pane's previous frame before recording.
        depth_.reset();
        ImageDesc desc;
        desc.format = kFormat;
        desc.width = frame.extent.width;
        desc.height = frame.extent.height;
        desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        auto created = Image::create(*context_, desc);
        if (!created) return std::unexpected(created.error().withContext("swapchain depth"));
        depth_ = std::move(*created);
    }
    transitionImage(commands, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color = vkStruct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    color.imageView = frame.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(color.clearValue.color.float32, clearColor, sizeof(float) * 4);
    VkRenderingAttachmentInfo depth = vkStruct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    depth.imageView = depth_.view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo info = vkStruct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    info.renderArea = {{0, 0}, frame.extent};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    info.pDepthAttachment = &depth;
    vkCmdBeginRendering(commands, &info);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, frame.extent};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    return {};
}

void SwapchainDepth::end(VkCommandBuffer commands) {
    vkCmdEndRendering(commands);
}

Result<Buffer> uploadBuffer(Context& context, UploadRing& ring, std::span<const std::byte> data, VkBufferUsageFlags usage,
                            bool flush) {
    auto buffer = Buffer::create(context, {std::max<VkDeviceSize>(data.size(), 4), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           MemoryUsage::GpuOnly});
    if (!buffer) return std::unexpected(buffer.error());
    if (data.empty()) return buffer;
    auto offset = ring.stage(data, 16);
    if (!offset) return std::unexpected(offset.error());
    auto commands = ring.commands();
    if (!commands) return std::unexpected(commands.error());
    const VkBufferCopy region{*offset, 0, data.size()};
    vkCmdCopyBuffer(*commands, ring.buffer(), buffer->handle(), 1, &region);
    VkMemoryBarrier2 barrier = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    VkDependencyInfo dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(*commands, &dep);
    if (!flush) return buffer;
    if (auto flushed = ring.flush(); !flushed) return std::unexpected(flushed.error());
    return buffer;
}

} // namespace apex::render
