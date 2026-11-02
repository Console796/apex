#include "render/renderer.hpp"
#include "render/clusters.hpp"

#include "core/log.hpp"
#include "render/render_target.hpp"
#include "render/gpu_lighting.hpp"
#include "render/scene_target.hpp"
#include "render/shadow_alloc.hpp"
#include "render/shadows.hpp"
#include "render/shader_blobs.hpp"
#include "render/swapchain.hpp"
#include "render/view_math.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
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
    DepthOnly, BoxDepthOnly, Overlay, WorldOverlay, WorldOccluded, GhostTextured, GhostFlat, Label, Tonemap, Highlight, Sky, Shadow, AddTextured, Count
};
constexpr std::size_t kKindCount = static_cast<std::size_t>(Kind::Count);

// Relative depth pull per AlphaMode layer (opaque 0, alpha test 1, blend 2, add 3), so coplanar
// alpha-tested and translucent faces win against the opaque face under them. Applied as a
// clip-z scale like the grid's surface bias: dynamic depth-bias units are implementation-defined.
// Well above D32 resolution (~6e-8) and far below the grid's 2e-4.
constexpr float kLayerBias = 1e-5f;

// Filled face kinds that honour DrawList::cullBackFaces. Wireframe's fill and depth passes never cull.
constexpr bool cullable(Kind kind) {
    return kind == Kind::Textured || kind == Kind::Flat || kind == Kind::GhostTextured ||
           kind == Kind::AddTextured || kind == Kind::GhostFlat || kind == Kind::Highlight;
}

bool srgbAttachment(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_SRGB:
    case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R8G8B8_SRGB:
    case VK_FORMAT_B8G8R8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return true;
    default: return false;
    }
}

enum class Input { Brush, Box, Sprite, None };

enum class Signature : std::uint32_t { Caller = 1, Scene = 2, Both = 3, Shadow = 4 };

struct Spec {
    Signature signatures;
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
    bool additive = false; // With blend: colour adds (ONE, ONE) and destination alpha is kept.
};

constexpr Spec kSpecs[kKindCount] = {
    {Signature::Both, "brush_vert", "brush_textured_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {Signature::Caller, "brush_vert", "brush_flat_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {Signature::Caller, "brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_LINE, true, false, true},
    {Signature::Caller, "brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, true, false, true},
    {Signature::Caller, "box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {Signature::Caller, "box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {Signature::Caller, "sprite_vert", "sprite_frag", Input::Sprite, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {Signature::Caller, "grid_vert", "grid_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, true},
    {Signature::Caller, "brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, true},
    {Signature::Caller, "brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, false, false},
    {Signature::Caller, "box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, true},
    {Signature::Caller, "box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, false, false},
    {Signature::Caller, "sprite_vert", "sprite_frag", Input::Sprite, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, true},
    {Signature::Caller, "sprite_vert", "sprite_frag", Input::Sprite, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, false, true, false, false},
    {Signature::Caller, "brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false, true, false},
    {Signature::Caller, "box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false, true, false},
    {Signature::Caller, "overlay_vert", "vcolor_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false, false},
    {Signature::Caller, "overlay_vert", "vcolor_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false},
    {Signature::Caller, "overlay_vert", "vcolor_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false, true, true, VK_COMPARE_OP_LESS},
    {Signature::Both, "brush_vert", "brush_textured_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false},
    {Signature::Caller, "brush_vert", "brush_flat_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false},
    {Signature::Caller, "overlay_vert", "label_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false, false},
    {Signature::Caller, "fullscreen_vert", "tonemap_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false, true, true, VK_COMPARE_OP_ALWAYS},
    {Signature::Caller, "brush_vert", "brush_textured_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false},
    {Signature::Both, "sky_vert", "sky_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, false, false, false},
    {Signature::Shadow, "brush_vert", "shadow_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, true, true, false},
    {Signature::Both, "brush_vert", "brush_textured_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false, true, true, VK_COMPARE_OP_GREATER_OR_EQUAL, true},
};

constexpr const char* kShaderIds[] = {"brush_vert", "brush_textured_frag", "brush_flat_frag", "solid_frag", "box_vert",
                                      "vcolor_frag", "sprite_vert", "sprite_frag", "grid_vert", "grid_frag", "overlay_vert", "label_frag", "fullscreen_vert", "tonemap_frag", "cluster_comp", "sky_vert", "sky_frag", "shadow_frag"};

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
    VkPipeline clusterPipeline = VK_NULL_HANDLE;
    struct ClusterSlot { Buffer lists, stats; };
    std::map<VkCommandBuffer, ClusterSlot> clusterBuffers; // Same fence discipline as frameBuffers.
    VkPipeline pipelines[3][kKindCount]{}; // Caller, HDR scene, depth-only shadow.
    struct ShadowSlot {
        std::unique_ptr<ShadowAtlas> atlas;
        std::vector<GpuShadowTile> tiles; // Uploaded order; a light's views are contiguous.
        std::vector<ShadowRect> rects;
        std::vector<ShadowReach> reach; // Per tile: casters beyond it are skipped.
        std::uint32_t overflow = 0; // In-view lights that did not fit the atlas (D7).
    };
    std::map<VkCommandBuffer, ShadowSlot> shadowSlots;
    std::map<const SceneTarget*, ShadowAllocator> shadowAllocators; // Per viewport: tiles follow its camera (D13).
    const SceneTarget* shadowOwner = nullptr; // Set by prepare for the upload it makes.
    std::map<VkCommandBuffer, std::array<TextureSlot, 2>> preparedScenes;
    std::map<std::string, std::vector<std::uint32_t>> overrides; // Hot-reloaded SPIR-V.
    // Swapchains have one frame in flight and distinct command buffers. A slot is reused only
    // after its fence, so host writes cannot race another viewport or an earlier submission.
    std::map<VkCommandBuffer, Buffer> frameBuffers;
    // The sky grid is immutable per revision: each slot re-uploads only when a different grid arrives.
    struct SkySlot {
        Buffer buffer;
        std::shared_ptr<const lighting::SkyVisibilityGrid> grid;
    };
    std::map<VkCommandBuffer, SkySlot> skyBuffers; // Same slot discipline as frameBuffers.
    struct MaterialSlot {
        Buffer buffer;
        std::uint64_t revision = ~std::uint64_t{0};
    };
    std::map<VkCommandBuffer, MaterialSlot> materialBuffers;
    std::shared_ptr<const GlyphAtlas> atlas;
    std::map<VkCommandBuffer, Buffer> overlayBuffers; // Same slot discipline as frameBuffers.
#ifdef APEX_SHADER_DIR
    std::map<std::string, std::filesystem::file_time_type> stamps;
#endif

    ~Impl() {
        if (!frameBuffers.empty() || !overlayBuffers.empty() || !skyBuffers.empty()) context->waitIdle();
        destroyPipelines();
        if (layout) vkDestroyPipelineLayout(context->device(), layout, nullptr);
    }

    void destroyPipelines() {
        if (clusterPipeline) vkDestroyPipeline(context->device(), clusterPipeline, nullptr);
        clusterPipeline = VK_NULL_HANDLE;
        for (auto& pass : pipelines) for (VkPipeline& pipeline : pass) {
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
    Result<VkPipeline> build(Kind kind, std::size_t pass) const {
        const bool scene = pass == 1, shadow = pass == 2;
        const Spec& spec = kSpecs[static_cast<std::size_t>(kind)];
        const auto signature = shadow ? Signature::Shadow : scene ? Signature::Scene : Signature::Caller;
        if (!(static_cast<std::uint32_t>(spec.signatures) & static_cast<std::uint32_t>(signature)))
            return VkPipeline(VK_NULL_HANDLE);
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
        raster.cullMode = VK_CULL_MODE_NONE; // Dynamic: set per bind and per mesh.
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
            if (spec.additive) {
                attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            }
        }
        VkPipelineColorBlendStateCreateInfo blend = vkStruct<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blend.attachmentCount = shadow ? 0u : 1u;
        blend.pAttachments = &attachment;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                                VK_DYNAMIC_STATE_CULL_MODE};
        VkPipelineDynamicStateCreateInfo dynamic = vkStruct<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount = 4;
        dynamic.pDynamicStates = dynamicStates;
        VkPipelineRenderingCreateInfo rendering = vkStruct<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
        rendering.colorAttachmentCount = shadow ? 0u : 1u;
        const VkFormat color = scene ? SceneTarget::kColorFormat : colorFormat;
        rendering.pColorAttachmentFormats = &color;
        rendering.depthAttachmentFormat = scene || shadow ? SceneTarget::kDepthFormat : depthFormat;

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
        VkPipeline built[3][kKindCount]{};
        for (std::size_t pass = 0; pass < 3; ++pass) for (std::size_t i = 0; i < kKindCount; ++i) {
            auto pipeline = build(static_cast<Kind>(i), pass);
            if (!pipeline) {
                for (auto& batch : built) for (VkPipeline p : batch) {
                    if (p) vkDestroyPipeline(context->device(), p, nullptr);
                }
                return std::unexpected(pipeline.error());
            }
            built[pass][i] = *pipeline;
        }
        auto shader = module("cluster_comp");
        VkPipeline compute = VK_NULL_HANDLE;
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (shader) {
            auto info = vkStruct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
            info.layout = layout;
            info.stage = vkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
            info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            info.stage.module = *shader;
            info.stage.pName = "main";
            result = vkCreateComputePipelines(context->device(), VK_NULL_HANDLE, 1, &info, nullptr, &compute);
            vkDestroyShaderModule(context->device(), *shader, nullptr);
        }
        if (result != VK_SUCCESS) {
            for (auto& batch : built) for (VkPipeline p : batch)
                if (p) vkDestroyPipeline(context->device(), p, nullptr);
            return std::unexpected(Error{ErrorCode::Internal, "cluster pipeline creation failed"});
        }
        destroyPipelines();
        clusterPipeline = compute;
        for (std::size_t pass = 0; pass < 3; ++pass)
            std::copy(std::begin(built[pass]), std::end(built[pass]), pipelines[pass]);
        return {};
    }

    void bind(VkCommandBuffer commands, Kind kind, bool scene = false) const {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[scene ? 1 : 0][static_cast<std::size_t>(kind)]);
        vkCmdSetCullMode(commands, VK_CULL_MODE_NONE); // drawMeshes culls the kinds that take it.
    }
    void pushAll(VkCommandBuffer commands, const PushConstants& push) const {
        vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_ALL, 0, sizeof(PushConstants), &push);
    }
    bool has(Kind kind, bool scene = false) const { return pipelines[scene ? 1 : 0][static_cast<std::size_t>(kind)] != VK_NULL_HANDLE; }
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

void Renderer::prepare(VkCommandBuffer commands, const DrawList& list, SceneTarget& scene, const float clearColor[4]) {
    if (list.mode != DisplayMode::Lit) return;
    impl_->shadowOwner = &scene;
    const bool uploaded = uploadFrame(commands, list, scene.extent());
    impl_->shadowOwner = nullptr;
    if (!uploaded) return;
    Impl& s = *impl_;
    const auto& buffer = s.clusterBuffers.at(commands).stats;
    vkCmdFillBuffer(commands, buffer.handle(), 0, 16, 0);
    auto barrier = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    auto dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dep);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.clusterPipeline);
    const VkDeviceAddress address = s.frameBuffers.at(commands).deviceAddress();
    auto push = basePush(list);
    push.p1.x = std::bit_cast<float>(static_cast<std::uint32_t>(address));
    push.p1.y = std::bit_cast<float>(static_cast<std::uint32_t>(address >> 32));
    s.pushAll(commands, push);
    const auto dims = clusterDimensions(scene.extent());
    vkCmdDispatch(commands, (dims.x * dims.y * dims.z + 63) / 64, 1, 1);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
    vkCmdPipelineBarrier2(commands, &dep);
    recordShadows(commands, list);
    // The clear colour is display-space, like the caller's clear (D15); decode it with the sRGB curve
    // the tone map encodes with, so it comes back unchanged.
    const auto decode = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    const float linearClear[4] = {decode(clearColor[0]), decode(clearColor[1]), decode(clearColor[2]), clearColor[3]};
    scene.begin(commands, linearClear);
    recordPass(commands, list, true, scene.extent());
    scene.end(commands);
    impl_->preparedScenes[commands] = {scene.colorSlot(), scene.depthSlot()};
}

void Renderer::recordShadows(VkCommandBuffer commands, const DrawList& list) {
    Impl& s = *impl_;
    const auto& shadow = s.shadowSlots.at(commands);
    if (shadow.tiles.empty()) return;
    std::uint32_t x0 = kShadowAtlasSize, y0 = kShadowAtlasSize, x1 = 0, y1 = 0;
    for (const auto& rect : shadow.rects) {
        x0 = std::min(x0, rect.x); y0 = std::min(y0, rect.y);
        x1 = std::max(x1, rect.x + rect.size); y1 = std::max(y1, rect.y + rect.size);
    }
    shadow.atlas->begin(commands, {{std::int32_t(x0), std::int32_t(y0)}, {x1 - x0, y1 - y0}});
    const VkDescriptorSet set = s.heap->descriptorSet();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &set, 0, nullptr);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      s.pipelines[2][static_cast<std::size_t>(Kind::Shadow)]);
    vkCmdSetCullMode(commands, VK_CULL_MODE_NONE); // Casters are drawn from both sides (dynamic state).
    vkCmdSetDepthBias(commands, kShadowConstantBias, 0, kShadowSlopeBias);
    const auto frame = s.frameBuffers.at(commands).deviceAddress();
    const auto& casters = list.shadowCasters ? *list.shadowCasters : list.meshes;
    // World boxes once per frame, one per instance; a draw casts into a view if any of them does (D13).
    std::vector<std::vector<AABB>> boxes(casters.size());
    for (std::size_t i = 0; i < casters.size(); ++i) {
        const auto& mesh = casters[i];
        if (mesh.bounds.empty()) continue;
        if (mesh.previewIndex == 0) {
            boxes[i].push_back({mesh.bounds.min + mesh.origin, mesh.bounds.max + mesh.origin});
            continue;
        }
        const std::uint32_t count = mesh.instancedTransforms ? mesh.instanceCount : 1;
        for (std::uint32_t k = 0; k < count; ++k) {
            const auto& preview = list.previews.at(mesh.previewIndex - 1 + k);
            boxes[i].push_back(worldBounds({mesh.bounds.min + preview.origin, mesh.bounds.max + preview.origin}, preview.matrix));
        }
    }
    for (std::size_t view = 0; view < shadow.tiles.size(); ++view) {
        ShadowAtlas::tile(commands, shadow.rects[view]);
        for (std::size_t i = 0; i < casters.size(); ++i) {
            const auto& mesh = casters[i];
            if (!mesh.vertices || !mesh.indices || mesh.indexCount == 0 || blended(mesh.alpha)) continue;
            if (!boxes[i].empty() && std::ranges::none_of(boxes[i], [&](const AABB& box) {
                    return castsInto(box, shadow.tiles[view].projection, list.view.cameraPosition, shadow.reach[view]);
                })) continue;
            const VkDeviceSize zero = 0;
            const VkBuffer vertices = mesh.vertices->handle();
            vkCmdBindVertexBuffers(commands, 0, 1, &vertices, &zero);
            vkCmdBindIndexBuffer(commands, mesh.indices->handle(), 0, VK_INDEX_TYPE_UINT32);
            auto push = basePush(list);
            push.viewProjection = shadow.tiles[view].projection;
            push.offsetSlot = slotVec(toCameraRelative(mesh.origin, list.view.cameraPosition), mesh.texture);
            push.colour = mesh.colour;
            const auto index = mesh.previewIndex | (mesh.instancedTransforms ? 0x80000000u : 0u);
            push.p1 = {std::bit_cast<float>(std::uint32_t(frame)), std::bit_cast<float>(std::uint32_t(frame >> 32)),
                       std::bit_cast<float>(index), std::bit_cast<float>(std::uint32_t(mesh.alpha))};
            s.pushAll(commands, push);
            vkCmdDrawIndexed(commands, mesh.indexCount, mesh.instanceCount, mesh.firstIndex, 0, 0);
        }
    }
    shadow.atlas->end(commands);
}

std::uint32_t Renderer::shadowOverflow(VkCommandBuffer commands) const {
    const auto found = impl_->shadowSlots.find(commands);
    return found == impl_->shadowSlots.end() ? 0 : found->second.overflow;
}

void Renderer::record(VkCommandBuffer commands, const DrawList& list) {
    if (list.mode != DisplayMode::Lit && !uploadFrame(commands, list)) return;
    recordPass(commands, list, false);
}

std::uint32_t Renderer::clusterOverflow(VkCommandBuffer commands) const {
    const auto found = impl_->clusterBuffers.find(commands);
    if (found == impl_->clusterBuffers.end()) return 0;
    found->second.stats.invalidate(0, sizeof(std::uint32_t));
    std::uint32_t value;
    std::memcpy(&value, found->second.stats.mapped(), sizeof(value));
    return value;
}

Result<assets::Texture> Renderer::buildCubemap(DrawList list, DVec3 origin, std::uint32_t size) {
    if (!std::has_single_bit(size) || size > 512) return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid cubemap size"});
    list.mode = DisplayMode::Lit;
    list.buildingCubemap = true;
    list.viewportSize = Vec2(static_cast<float>(size));
    list.view.cameraPosition = origin;
    const DVec3 forward[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const DVec3 up[] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    SceneTarget target(*impl_->context, *impl_->heap);
    if (auto sized = target.resize(size, size); !sized) return std::unexpected(sized.error());
    assets::Texture result;
    result.format = assets::TextureFormat::Rgba32Float;
    result.faces = 6;
    assets::TextureMip mip;
    mip.width = mip.height = static_cast<std::uint16_t>(size);
    for (int face = 0; face < 6; ++face) {
        const DVec3 right = glm::cross(forward[face], up[face]);
        list.view.viewProjection = reversedZPerspective(glm::radians(90.0), 1.0, 1.0) *
            rotationOnlyView(forward[face], right, up[face]);
        list.view.cameraRight = Vec3(right);
        list.view.cameraUp = Vec3(up[face]);
        auto rendered = impl_->context->oneShot([&](VkCommandBuffer commands) {
            const float clear[4] = {0, 0, 0, 1};
            prepare(commands, list, target, clear);
            impl_->preparedScenes.erase(commands);
        });
        if (!rendered) return std::unexpected(rendered.error());
        auto pixels = target.readback();
        if (!pixels) return std::unexpected(pixels.error());
        auto bytes = std::as_bytes(std::span(*pixels));
        mip.data.insert(mip.data.end(), bytes.begin(), bytes.end());
    }
    result.mips.push_back(std::move(mip));
    while (size > 1) {
        const auto next = size / 2;
        assets::TextureMip level;
        level.width = level.height = static_cast<std::uint16_t>(next);
        std::vector<float> pixels(6 * next * next * 4);
        const auto& source = result.mips.back().data;
        for (std::size_t face = 0; face < 6; ++face) for (std::size_t y = 0; y < next; ++y)
            for (std::size_t x = 0; x < next; ++x) for (std::size_t c = 0; c < 4; ++c) {
                float value = 0;
                for (std::size_t dy = 0; dy < 2; ++dy) for (std::size_t dx = 0; dx < 2; ++dx) {
                    float sample;
                    const auto index = ((face * size * size + (2 * y + dy) * size + 2 * x + dx) * 4 + c) * sizeof(float);
                    std::memcpy(&sample, source.data() + index, sizeof(sample));
                    value += sample * 0.25f;
                }
                pixels[(face * next * next + y * next + x) * 4 + c] = value;
            }
        const auto bytes = std::as_bytes(std::span(pixels));
        level.data.assign(bytes.begin(), bytes.end());
        result.mips.push_back(std::move(level));
        size = next;
    }
    return result;
}

bool Renderer::uploadFrame(VkCommandBuffer commands, const DrawList& list, VkExtent2D extent) {
    Impl& s = *impl_;
    const VkDeviceSize lightOffset = sizeof(FrameBlock);
    const VkDeviceSize previewOffset = lightOffset + list.lights.size() * sizeof(GpuLight);
    const VkDeviceSize cubemapOffset = previewOffset + list.previews.size() * sizeof(PreviewTransform);
    // At least one probe slot, so frame.cubemaps is never a null address: llvmpipe's JIT dereferences it in
    // nearestCubemap even when cubemapCount is 0. The zeroed slot names the default cube.
    const VkDeviceSize shadowOffset = cubemapOffset + std::max<std::size_t>(list.cubemaps.size(), 1) * sizeof(GpuCubemapProbe);
    const VkDeviceSize size = shadowOffset + kMaxShadowViews * sizeof(GpuShadowTile);
    const auto* sky = list.skyVisibility.get();
    VkDeviceAddress skyAddress = 0;
    if (sky && sky->probeCount() > 0) {
        Impl::SkySlot& slot = s.skyBuffers[commands];
        if (slot.grid != list.skyVisibility) {
            const VkDeviceSize skySize = sky->probeCount() * sizeof(GpuSkyProbe);
            if (slot.buffer.size() < skySize) {
                auto created = Buffer::create(*s.context, {skySize,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
                if (!created) {
                    log::error("sky buffer: {}", created.error().what());
                    return false;
                }
                slot.buffer = std::move(*created);
            }
            auto* probes = static_cast<std::byte*>(slot.buffer.mapped());
            const auto dims = sky->dims();
            std::size_t index = 0;
            for (int z = 0; z < dims.z; ++z)
                for (int y = 0; y < dims.y; ++y)
                    for (int x = 0; x < dims.x; ++x) {
                        const auto packed = packSkyProbe(sky->probe(x, y, z));
                        std::memcpy(probes + index++ * sizeof(GpuSkyProbe), &packed, sizeof(packed));
                    }
            slot.buffer.flush(0, skySize);
            slot.grid = list.skyVisibility;
        }
        skyAddress = slot.buffer.deviceAddress();
    }
    VkDeviceAddress materialAddress = 0;
    const auto materials = s.heap->materials();
    if (list.mode == DisplayMode::Lit && !materials.empty()) {
        auto& slot = s.materialBuffers[commands];
        if (slot.revision != s.heap->materialRevision()) {
            const auto bytes = materials.size_bytes();
            if (slot.buffer.size() < bytes) {
                auto created = Buffer::create(*s.context, {bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
                if (!created) {
                    log::error("material buffer: {}", created.error().what());
                    return false;
                }
                slot.buffer = std::move(*created);
            }
            std::memcpy(slot.buffer.mapped(), materials.data(), bytes);
            slot.buffer.flush(0, bytes);
            slot.revision = s.heap->materialRevision();
        }
        materialAddress = slot.buffer.deviceAddress();
    }
    Buffer& buffer = s.frameBuffers[commands];
    if (buffer.size() < size) {
        auto created = Buffer::create(*s.context, {std::max(size, buffer.size() * 2),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
        if (!created) {
            log::error("frame buffer: {}", created.error().what());
            return false;
        }
        buffer = std::move(*created);
    }
    auto* bytes = static_cast<std::byte*>(buffer.mapped());
    FrameBlock frame;
    auto& shadow = s.shadowSlots[commands];
    shadow.tiles.clear();
    shadow.rects.clear();
    shadow.reach.clear();
    shadow.overflow = 0;
    // Tile planning (D13): the allocator keeps owners across frames; the atlas itself is redrawn every frame.
    std::vector<ShadowGrant> grants(list.lights.size());
    const auto exposureRange = [&](const lighting::Light& light) {
        return light.kind == lighting::LightKind::Sun || light.kind == lighting::LightKind::Ambient
            ? 0.0 : lighting::previewRange(light, list.exposure);
    };
    if (list.mode == DisplayMode::Lit && !list.fullbright && s.shadowOwner &&
        !(list.shadowCasters ? list.shadowCasters->empty() : list.meshes.empty())) {
        std::vector<ShadowRequest> requests;
        std::vector<std::size_t> requested;
        for (std::size_t i = 0; i < list.lights.size(); ++i)
            if (auto request = shadowRequest(list.lights[i], list.view.cameraPosition, list.view.viewProjection,
                                             exposureRange(list.lights[i]))) {
                requests.push_back(*request);
                requested.push_back(i);
            }
        static const auto epoch = std::chrono::steady_clock::now();
        auto& allocator = s.shadowAllocators[s.shadowOwner];
        const auto result = allocator.update(requests, std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count());
        shadow.overflow = allocator.overflow();
        bool any = false;
        for (std::size_t k = 0; k < requested.size(); ++k) {
            grants[requested[k]] = result[k];
            any = any || result[k].shadowed;
        }
        if (any && !shadow.atlas) {
            auto atlas = std::make_unique<ShadowAtlas>(*s.context, *s.heap);
            auto created = atlas->create();
            if (!created) { log::error("shadow atlas: {}", created.error().what()); return false; }
            shadow.atlas = std::move(atlas);
        }
    }
    if (list.mode == DisplayMode::Lit) {
        const auto dims = clusterDimensions(extent);
        const VkDeviceSize clusterSize = VkDeviceSize(dims.x) * dims.y * dims.z * sizeof(GpuCluster);
        auto& clusters = s.clusterBuffers[commands];
        if (clusters.lists.size() < clusterSize) {
            auto created = Buffer::create(*s.context, {clusterSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::GpuOnly});
            if (!created) { log::error("cluster buffer: {}", created.error().what()); return false; }
            clusters.lists = std::move(*created);
        }
        if (!clusters.stats) {
            auto created = Buffer::create(*s.context, {16,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback});
            if (!created) { log::error("cluster counter: {}", created.error().what()); return false; }
            clusters.stats = std::move(*created);
        }
        frame.clusters = clusters.lists.deviceAddress();
        frame.clusterStats = clusters.stats.deviceAddress();
        frame.clusterDims = glm::uvec4(dims, 0);
        frame.clusterProjection = list.view.viewProjection;
    }
    frame.previews = buffer.deviceAddress() + previewOffset;
    frame.lights = buffer.deviceAddress() + lightOffset;
    frame.lightCount = static_cast<std::uint32_t>(list.lights.size());
    frame.flags = (list.fullbright ? 1u : 0u) | (list.buildingCubemap ? 2u : 0u);
    frame.fogColour = Vec4(list.fog.colour, 0);
    frame.fill = Vec4(list.ambientFill, 0.0f, 0.0f, 0.0f);
    frame.fogParams = Vec4(list.fog.start, list.fog.end, list.fog.maxDensity, list.fog.enabled ? 1.0f : 0.0f);
    frame.skyInverseView = glm::inverse(list.view.viewProjection);
    for (std::size_t i = 0; i < 6; ++i) frame.skyFaces[i / 4][i % 4] = list.sky.faces[i];
    frame.skyFaces[1].z = list.sky.linearMask;
    if (list.sky.camera) frame.skyTransform = Vec4(Vec3(
        (list.view.cameraPosition - *list.sky.camera) * list.sky.scale - list.view.cameraPosition), float(list.sky.scale));
    frame.materials = materialAddress;
    frame.materialCount = materialAddress ? static_cast<std::uint32_t>(materials.size()) : 0;
    frame.cubemaps = buffer.deviceAddress() + cubemapOffset;
    frame.cubemapCount = static_cast<std::uint32_t>(list.cubemaps.size());
    frame.cameraRight = Vec4(list.view.cameraRight, 0);
    frame.cameraUp = Vec4(list.view.cameraUp, 0);
    if (skyAddress) packSkyGrid(frame, *sky, skyAddress, list.view.cameraPosition);
    frame.shadows = buffer.deviceAddress() + shadowOffset;
    std::size_t lightIndex = 0;
    for (const bool global : {true, false}) for (std::size_t i = 0; i < list.lights.size(); ++i) {
        const auto& light = list.lights[i];
        const bool isGlobal = light.kind == lighting::LightKind::Sun || light.kind == lighting::LightKind::Ambient;
        if (isGlobal != global) continue;
        auto packed = packLight(light, list.view.cameraPosition);
        if (!global && list.mode == DisplayMode::Lit)
            packed.originRadius.w = static_cast<float>(lighting::previewRange(light, list.exposure));
        if (const auto& grant = grants[i]; grant.shadowed) {
            std::vector<GpuShadowTile> views;
            const auto rects = std::span<const ShadowRect>(grant.rects.data(), grant.views);
            if (light.kind == lighting::LightKind::Sun)
                views = sunCascadeTiles(light.direction, list.view.viewProjection, list.view.cameraPosition, rects);
            else for (std::uint32_t v = 0; v < grant.views; ++v)
                views.push_back(grant.views == 6 ? pointShadowTile(light, list.view.cameraPosition, v, rects[v])
                                                 : spotShadowTile(light, list.view.cameraPosition, rects[v]));
            if (!views.empty()) {
                packed.shadow = static_cast<std::uint32_t>(shadow.tiles.size()) + 1;
                packed.shadowViews = static_cast<std::uint32_t>(views.size());
                const auto reach = light.kind == lighting::LightKind::Sun
                    ? ShadowReach{} : ShadowReach{light.origin, exposureRange(light)};
                for (std::size_t v = 0; v < views.size(); ++v) {
                    shadow.tiles.push_back(views[v]);
                    shadow.rects.push_back(rects[v]);
                    shadow.reach.push_back(reach);
                }
            }
        }
        std::memcpy(bytes + lightOffset + lightIndex++ * sizeof(GpuLight), &packed, sizeof(packed));
        if (global) ++frame.clusterDims.w;
    }
    if (!shadow.tiles.empty()) {
        frame.shadowAtlas = shadow.atlas->slot();
        frame.shadowCount = static_cast<std::uint32_t>(shadow.tiles.size());
        std::memcpy(bytes + shadowOffset, shadow.tiles.data(), shadow.tiles.size() * sizeof(GpuShadowTile));
    }
    std::memcpy(bytes, &frame, sizeof(frame));
    for (std::size_t i = 0; i < list.previews.size(); ++i) {
        const auto& preview = list.previews[i];
        const auto packed = cameraRelativePreview(preview.matrix, preview.origin, list.view.cameraPosition);
        std::memcpy(bytes + previewOffset + i * sizeof(PreviewTransform), &packed, sizeof(packed));
    }
    for (std::size_t i = 0; i < list.cubemaps.size(); ++i) {
        const Vec3 position(list.cubemaps[i].position - list.view.cameraPosition);
        GpuCubemapProbe probe{{position.x, position.y, position.z}, list.cubemaps[i].cube};
        if (const auto& box = list.cubemaps[i].correction) {
            DMat4 relative = *box;
            relative[3] -= DVec4(list.view.cameraPosition, 0);
            probe.worldToBox = Mat4(glm::inverse(relative));
        }
        std::memcpy(bytes + cubemapOffset + i * sizeof(GpuCubemapProbe), &probe, sizeof(probe));
    }
    if (list.cubemaps.empty()) std::memset(bytes + cubemapOffset, 0, sizeof(GpuCubemapProbe));
    buffer.flush(0, size);
    return true;
}

void Renderer::recordPass(VkCommandBuffer commands, const DrawList& list, bool scenePass, VkExtent2D passExtent) {
    Impl& s = *impl_;
    const VkDeviceAddress frameAddress = s.frameBuffers.at(commands).deviceAddress();
    const auto previewPush = [&](PushConstants& push, std::uint32_t index, bool sprite = false, bool outline = false) {
        assert(index <= list.previews.size());
        const float low = std::bit_cast<float>(static_cast<std::uint32_t>(frameAddress));
        const float high = std::bit_cast<float>(static_cast<std::uint32_t>(frameAddress >> 32));
        if (sprite) {
            assert(index <= 0x7FFFFFFFu);
            push.p1.x = low;
            push.p1.y = high;
            push.p0.w = std::bit_cast<float>((index << 1) | (outline ? 1u : 0u));
        } else {
            push.p1 = Vec4(low, high, std::bit_cast<float>(index), 0.0f);
        }
    };
    const VkDescriptorSet set = s.heap->descriptorSet();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &set, 0, nullptr);
    vkCmdSetDepthBias(commands, 0.0f, 0.0f, 0.0f);
    const DVec3 camera = list.view.cameraPosition;

    bool skyPass = false;
    auto drawMeshes = [&](Kind kind, const auto& meshes, auto&& setup, auto&& count, auto&& indices, bool edges) {
        if (!s.has(kind, scenePass)) return;
        s.bind(commands, kind, scenePass);
        // Tool ghosts stay double-sided so a preview reads as a volume; translucent materials cull.
        const bool cull = list.cullBackFaces && cullable(kind) &&
                          static_cast<const void*>(&meshes) != static_cast<const void*>(&list.ghosts);
        for (const MeshDraw& mesh : meshes) {
            const std::uint32_t n = count(mesh);
            if (!mesh.vertices || !indices(mesh) || n == 0 ||
                ((scenePass || list.mode == DisplayMode::Textured) &&
                 (kind == Kind::Textured || kind == Kind::GhostTextured || kind == Kind::AddTextured) && (mesh.skySurface || mesh.sky != skyPass))) continue;
            if (cull) {
                // Brushes and studio models share one winding; a mirroring preview transform flips
                // which side is the back.
                const bool mirrored = mesh.previewIndex != 0 &&
                                      glm::determinant(DMat3(list.previews[mesh.previewIndex - 1].matrix)) < 0.0;
                vkCmdSetCullMode(commands, mirrored ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
            }
            const VkDeviceSize zero = 0;
            VkBuffer vb = mesh.vertices->handle();
            vkCmdBindVertexBuffers(commands, 0, 1, &vb, &zero);
            vkCmdBindIndexBuffer(commands, indices(mesh)->handle(), 0, VK_INDEX_TYPE_UINT32);
            PushConstants push = basePush(list);
            push.offsetSlot = slotVec(toCameraRelative(mesh.origin, camera), mesh.texture);
            setup(push, mesh);
            previewPush(push, mesh.previewIndex);
            if (mesh.instancedTransforms) push.p1.z = std::bit_cast<float>(mesh.previewIndex | 0x80000000u);
            // Bit 2 asks brush_textured_frag for the tint alone (Lit's post-tone-map highlight pass).
            push.p1.w = std::bit_cast<float>(static_cast<std::uint32_t>(mesh.alpha) |
                                            (kind == Kind::Highlight ? 4u : 0u) | (scenePass ? 8u : 0u) | (skyPass ? 16u : 0u));
            // Every pass of a mesh shares its layer's bias so its edges and outlines stay on its faces.
            const float layerScale = 1.0f + kLayerBias * static_cast<float>(mesh.alpha);
            for (int c = 0; c < 4; ++c) push.viewProjection[c][2] *= layerScale;
            s.pushAll(commands, push);
            vkCmdDrawIndexed(commands, n, mesh.instanceCount, edges ? mesh.firstEdgeIndex : mesh.firstIndex, 0, 0);
        }
    };
    auto trianglesOf = [](const MeshDraw& m) { return m.indexCount; };
    auto trianglesBuf = [](const MeshDraw& m) { return m.indices; };
    auto edgesOf = [](const MeshDraw& m) { return m.edgeIndexCount; };
    auto edgesBuf = [](const MeshDraw& m) { return m.edgeIndices; };
    auto brushColour = [&](PushConstants& p, const MeshDraw& m) {
        p.colour = m.colour;
        p.p0 = scenePass && m.reflectionPoint ? Vec4(toCameraRelative(*m.reflectionPoint, camera), 1) :
            scenePass ? Vec4(0.0f) : m.highlight; // Lit tints after tone mapping (D15).
    };

    if (!scenePass && list.mode == DisplayMode::Lit) {
        const auto prepared = s.preparedScenes.find(commands);
        assert(prepared != s.preparedScenes.end());
        if (prepared == s.preparedScenes.end()) return;
        PushConstants push{};
        push.viewProjection = list.view.viewProjection; // The AO reads the projection's scale and near plane.
        push.colour = Vec4(list.aoStrength, list.aoRadius, 0.0f, 0.0f);
        push.offsetSlot.w = std::bit_cast<float>(prepared->second[0]);
        push.p0.x = std::bit_cast<float>(prepared->second[1]);
        push.p0.y = list.exposure;
        push.p0.z = std::bit_cast<float>(srgbAttachment(s.colorFormat) ? 1u : 0u);
        s.bind(commands, Kind::Tonemap);
        s.pushAll(commands, push);
        vkCmdDraw(commands, 3, 1, 0, 0);
        s.preparedScenes.erase(prepared);
        // Selection and hover tints, depth-tested against the copied scene depth.
        drawMeshes(Kind::Highlight, list.meshes, brushColour,
                   [](const MeshDraw& m) { return m.highlight.a > 0.0f ? m.indexCount : 0u; }, trianglesBuf, false);
    }
    if ((scenePass || list.mode == DisplayMode::Textured) && list.sky.enabled) {
        PushConstants push = basePush(list);
        previewPush(push, 0);
        push.p0.x = scenePass ? 1.0f : 0.0f;
        s.bind(commands, Kind::Sky, scenePass);
        s.pushAll(commands, push);
        vkCmdDraw(commands, 3, 1, 0, 0);
    }
    if ((scenePass || list.mode == DisplayMode::Textured) && list.sky.camera) {
        skyPass = true;
        drawMeshes(Kind::Textured, list.meshes, brushColour,
            [](const MeshDraw& m) { return blended(m.alpha) ? 0u : m.indexCount; }, trianglesBuf, false);
        drawMeshes(Kind::GhostTextured, list.meshes, brushColour,
            [](const MeshDraw& m) { return m.alpha == AlphaMode::Blend ? m.indexCount : 0u; }, trianglesBuf, false);
        drawMeshes(Kind::AddTextured, list.meshes, brushColour,
            [](const MeshDraw& m) { return m.alpha == AlphaMode::Add ? m.indexCount : 0u; }, trianglesBuf, false);
        // Main geometry always wins over the 3D sky (D10).
        VkClearAttachment depth{};
        depth.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth.clearValue.depthStencil = {0.0f, 0};
        if (!passExtent.width) passExtent = {static_cast<std::uint32_t>(list.viewportSize.x), static_cast<std::uint32_t>(list.viewportSize.y)};
        const VkClearRect rect{{{0, 0}, passExtent}, 0, 1};
        if (rect.rect.extent.width && rect.rect.extent.height) vkCmdClearAttachments(commands, 1, &depth, 1, &rect);
        skyPass = false;
    }
    switch (list.mode) {
    case DisplayMode::Lit:
        if (!scenePass) break;
        [[fallthrough]];
    case DisplayMode::Textured: {
        const auto opaqueOf = [](const MeshDraw& m) { return blended(m.alpha) ? 0u : m.indexCount; };
        drawMeshes(Kind::Textured, list.meshes, brushColour, opaqueOf, trianglesBuf, false);
        // Translucent solids last, farthest first (by draw origin), blended over the opaque scene.
        std::vector<MeshDraw> translucent;
        for (const MeshDraw& mesh : list.meshes) {
            if (mesh.alpha == AlphaMode::Blend) translucent.push_back(mesh);
        }
        const auto distance = [&](const MeshDraw& m) { return glm::dot(m.origin - camera, m.origin - camera); };
        std::ranges::stable_sort(translucent, [&](const MeshDraw& a, const MeshDraw& b) { return distance(a) > distance(b); });
        drawMeshes(Kind::GhostTextured, translucent, brushColour, trianglesOf, trianglesBuf, false);
        // Additive layers commute, so they need no sort.
        drawMeshes(Kind::AddTextured, list.meshes, brushColour,
            [](const MeshDraw& m) { return m.alpha == AlphaMode::Add ? m.indexCount : 0u; }, trianglesBuf, false);
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
    if (scenePass) return;
    if (list.mode != DisplayMode::Wireframe)
        drawMeshes(list.mode == DisplayMode::Textured || list.mode == DisplayMode::Lit ? Kind::GhostTextured : Kind::GhostFlat,
                   list.ghosts, brushColour, trianglesOf, trianglesBuf, false);
    if (list.edgeOverlay && list.mode != DisplayMode::Wireframe && s.has(Kind::Edges)) {
        vkCmdSetDepthBias(commands, list.edgeDepthBias, 0.0f, 1.0f);
        drawMeshes(Kind::Edges, list.meshes, [&](PushConstants& p, const MeshDraw&) { p.colour = list.edgeColour; },
                   [](const MeshDraw& m) { return m.edgeOverlay ? m.edgeIndexCount : 0u; }, edgesBuf, true);
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
