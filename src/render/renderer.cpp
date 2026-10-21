#include "render/renderer.hpp"

#include "core/log.hpp"
#include "render/render_target.hpp"
#include "render/shader_blobs.hpp"
#include "render/swapchain.hpp"
#include "render/view_math.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace apex::render {

namespace {

enum class Kind : std::uint32_t { Textured, Flat, WireFill, Edges, BoxSolid, BoxWire, Sprite, Grid, Count };
constexpr std::size_t kKindCount = static_cast<std::size_t>(Kind::Count);

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
};

constexpr Spec kSpecs[kKindCount] = {
    {"brush_vert", "brush_textured_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {"brush_vert", "brush_flat_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {"brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_LINE, true, false, true},
    {"brush_vert", "solid_frag", Input::Brush, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, true, false, true},
    {"box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {"box_vert", "vcolor_frag", Input::Box, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {"sprite_vert", "sprite_frag", Input::Sprite, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, true, false, false},
    {"grid_vert", "grid_frag", Input::None, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_POLYGON_MODE_FILL, false, true, false},
};

constexpr const char* kShaderIds[] = {"brush_vert", "brush_textured_frag", "brush_flat_frag", "solid_frag", "box_vert",
                                      "vcolor_frag", "sprite_vert", "sprite_frag", "grid_vert", "grid_frag"};

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
#ifdef APEX_SHADER_DIR
    std::map<std::string, std::filesystem::file_time_type> stamps;
#endif

    ~Impl() {
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
                          {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(BoxInstance, colour)}};
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
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = spec.depthWrite ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // Reversed-Z.
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    const VkDescriptorSet set = s.heap->descriptorSet();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &set, 0, nullptr);
    vkCmdSetDepthBias(commands, 0.0f, 0.0f, 0.0f);
    const DVec3 camera = list.view.cameraPosition;

    auto drawMeshes = [&](Kind kind, auto&& setup, auto&& count, auto&& indices) {
        if (!s.has(kind)) return;
        s.bind(commands, kind);
        for (const MeshDraw& mesh : list.meshes) {
            const std::uint32_t n = count(mesh);
            if (!mesh.vertices || !indices(mesh) || n == 0) continue;
            const VkDeviceSize zero = 0;
            VkBuffer vb = mesh.vertices->handle();
            vkCmdBindVertexBuffers(commands, 0, 1, &vb, &zero);
            vkCmdBindIndexBuffer(commands, indices(mesh)->handle(), 0, VK_INDEX_TYPE_UINT32);
            PushConstants push = basePush(list);
            push.offsetSlot = slotVec(toCameraRelative(mesh.origin, camera), mesh.texture);
            setup(push, mesh);
            s.pushAll(commands, push);
            vkCmdDrawIndexed(commands, n, 1, 0, 0, 0);
        }
    };
    auto trianglesOf = [](const MeshDraw& m) { return m.indexCount; };
    auto trianglesBuf = [](const MeshDraw& m) { return m.indices; };
    auto edgesOf = [](const MeshDraw& m) { return m.edgeIndexCount; };
    auto edgesBuf = [](const MeshDraw& m) { return m.edgeIndices; };

    switch (list.mode) {
    case DisplayMode::Textured:
        drawMeshes(Kind::Textured, [](PushConstants& p, const MeshDraw& m) { p.colour = m.colour; }, trianglesOf, trianglesBuf);
        break;
    case DisplayMode::Flat:
        drawMeshes(Kind::Flat, [](PushConstants& p, const MeshDraw& m) { p.colour = m.colour; }, trianglesOf, trianglesBuf);
        break;
    case DisplayMode::Wireframe: {
        // Prefer the real edge list; fall back to polygon-mode lines (shows triangulation).
        bool anyEdges = std::ranges::any_of(list.meshes, [](const MeshDraw& m) { return m.edgeIndices && m.edgeIndexCount; });
        if (anyEdges) {
            drawMeshes(Kind::Edges, [](PushConstants& p, const MeshDraw& m) { p.colour = m.colour; }, edgesOf, edgesBuf);
        } else {
            drawMeshes(Kind::WireFill, [](PushConstants& p, const MeshDraw& m) { p.colour = m.colour; }, trianglesOf, trianglesBuf);
        }
        break;
    }
    }
    if (list.edgeOverlay && list.mode != DisplayMode::Wireframe && s.has(Kind::Edges)) {
        vkCmdSetDepthBias(commands, list.edgeDepthBias, 0.0f, 1.0f);
        drawMeshes(Kind::Edges, [&](PushConstants& p, const MeshDraw&) { p.colour = list.edgeColour; }, edgesOf, edgesBuf);
        vkCmdSetDepthBias(commands, 0.0f, 0.0f, 0.0f);
    }

    for (const BoxDraw& box : list.boxes) {
        if (!box.instances || box.count == 0) continue;
        const Kind kind = box.solid ? Kind::BoxSolid : Kind::BoxWire;
        s.bind(commands, kind);
        const VkDeviceSize zero = 0;
        VkBuffer vb = box.instances->handle();
        vkCmdBindVertexBuffers(commands, 0, 1, &vb, &zero);
        PushConstants push = basePush(list);
        push.offsetSlot = slotVec(toCameraRelative(box.origin, camera), 0);
        push.p0 = Vec4(box.solid ? 1.0f : 0.0f);
        s.pushAll(commands, push);
        vkCmdDraw(commands, box.solid ? 36 : 24, box.count, 0, 0);
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
            push.p0 = Vec4(list.view.cameraRight, 0.0f);
            push.p1 = Vec4(list.view.cameraUp, 0.0f);
            s.pushAll(commands, push);
            vkCmdDraw(commands, 6, sprite.count, 0, 0);
        }
    }

    if (list.grid.enabled) {
        s.bind(commands, Kind::Grid);
        PushConstants push = basePush(list);
        push.colour = list.grid.colour;
        // Camera origin modulo 1024 (exact in float; the spacing divides 1024) keeps the grid lines
        // stable however far the camera is from the world origin.
        auto wrap = [](double v) { return static_cast<float>(v - std::floor(v / 1024.0) * 1024.0); };
        push.p0 = Vec4(wrap(camera.x), wrap(camera.y), wrap(camera.z), list.grid.spacing);
        push.p1 = Vec4(list.grid.fadeDistance, static_cast<float>(list.grid.planeZ - camera.z), 0.0f, 0.0f);
        s.pushAll(commands, push);
        vkCmdDraw(commands, 6, 1, 0, 0);
    }
}

Result<void> SwapchainDepth::begin(VkCommandBuffer commands, const SwapchainFrame& frame, const float clearColor[4]) {
    if (!depth_ || depth_.extent().width != frame.extent.width || depth_.extent().height != frame.extent.height) {
        context_->waitIdle();
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

Result<Buffer> uploadBuffer(Context& context, UploadRing& ring, std::span<const std::byte> data, VkBufferUsageFlags usage) {
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
    if (auto flushed = ring.flush(); !flushed) return std::unexpected(flushed.error());
    return buffer;
}

} // namespace apex::render
