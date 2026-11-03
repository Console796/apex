#include "render/probe_volume.hpp"

#include "lighting/cpu_accel.hpp"
#include "render/scene_accel.hpp"
#include "render/shader_blobs.hpp"
#include "render/view_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace apex::render {
namespace {
struct Triangle { Vec4 a, b, c, albedo; };
struct UpdatePush {
    VkDeviceAddress previous, output, triangles, offsets, lights;
    std::uint32_t lightCount, first;
    glm::uvec4 dims;
    Vec4 originSpacing, params;
    VkDeviceAddress context; // CascadeContext above, shared by both cascades' dispatches.
};
static_assert(sizeof(UpdatePush) == 104);

// Shared by both dispatches: previous-frame geometry of both grids, so each probe's transported
// bounce blends the fine and coarse history exactly like the runtime sampler.
struct CascadeContext {
    glm::uvec4 fineDims{0};        // xyz fine dimensions; w unused.
    Vec4 fineOriginSpacing{0};     // xyz fine origin; w fine spacing.
    Vec4 fineParams{0};            // x fine normal bias; y fine fade cells.
    glm::uvec4 addresses{0};       // xy fine previous map; zw coarse previous map.
    glm::uvec4 coarseDims{0};      // xyz coarse dimensions; w unused.
    Vec4 coarseOriginSpacing{0};   // xyz coarse origin; w coarse spacing.
    Vec4 coarseParams{0};          // x coarse bias; y relocate; z clearance fraction; w max fraction.
};
static_assert(sizeof(CascadeContext) == 112);

Result<void> upload(Context& context, Buffer& buffer, const void* data, std::size_t size) {
    if (buffer.size() < size) {
        auto created = Buffer::create(context, {size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
        if (!created) return std::unexpected(created.error());
        buffer = std::move(*created);
    }
    if (size) { std::memcpy(buffer.mapped(), data, size); buffer.flush(); }
    return {};
}
void barrier(VkCommandBuffer commands, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess) {
    auto memory = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    memory.srcStageMask = src; memory.srcAccessMask = srcAccess;
    memory.dstStageMask = dst; memory.dstAccessMask = dstAccess;
    auto dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(commands, &dep);
}
}
struct ProbeVolume::Impl {
    explicit Impl(Context& ctx) : context(ctx), scene(&ctx) {}
    Context& context;
    SceneAccel scene;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    Buffer maps[2], coarseMaps[2], triangles, offsets, lights, cascadeContext;
    std::vector<GpuLight> lastLights;
    std::vector<GpuMaterial> lastMaterials;
    std::vector<std::uint64_t> lastSurfaces;
    ProbeSettings settings;
    Vec3 origin{0};
    glm::uvec3 coarseDims{0};   // No coarse cascade when cascades are disabled.
    Vec3 coarseOrigin{0};
    std::uint32_t current = 0, cursor = 0, refresh = 0;
    std::uint32_t coarseCursor = 0, coarseRefresh = 0;
    bool clear = true, ready = false;
    static glm::uvec3 cascadeDims(glm::uvec3 fine) { // max(dims/2, 2); the coarse extent grows by the spacing scale.
        return glm::max(fine / 2u, glm::uvec3(2));
    }
    float cascadeSpacing() const { return settings.spacing * settings.cascadeScale; }
    float cascadeBias() const { return settings.normalBias * settings.cascadeScale; }
    ~Impl() {
        if (pipeline) vkDestroyPipeline(context.device(), pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(context.device(), layout, nullptr);
        if (pool) vkDestroyDescriptorPool(context.device(), pool, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(context.device(), setLayout, nullptr);
    }
};
ProbeVolume::ProbeVolume(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProbeVolume::~ProbeVolume() = default;
Result<std::unique_ptr<ProbeVolume>> ProbeVolume::create(Context& context) {
    if (!context.info().rayQuery)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "realtime indirect requires ray query"});
    auto s = std::make_unique<Impl>(context);
    const auto device = context.device();
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                                               VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    auto setInfo = vkStruct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    setInfo.bindingCount = 1; setInfo.pBindings = &binding;
    APEX_VK_TRY(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &s->setLayout), "probe set layout");
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
    auto poolInfo = vkStruct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    poolInfo.maxSets = 1; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
    APEX_VK_TRY(vkCreateDescriptorPool(device, &poolInfo, nullptr, &s->pool), "probe pool");
    auto allocate = vkStruct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = s->pool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &s->setLayout;
    APEX_VK_TRY(vkAllocateDescriptorSets(device, &allocate, &s->set), "probe set");
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpdatePush)};
    auto layout = vkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.setLayoutCount = 1; layout.pSetLayouts = &s->setLayout;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
    APEX_VK_TRY(vkCreatePipelineLayout(device, &layout, nullptr, &s->layout), "probe pipeline layout");
    const auto code = builtinSpirv("probe_update_comp");
    auto module = vkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    module.codeSize = code.size_bytes(); module.pCode = code.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    APEX_VK_TRY(vkCreateShaderModule(device, &module, nullptr, &shader), "probe shader");
    auto pipeline = vkStruct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipeline.layout = s->layout;
    pipeline.stage = vkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = shader; pipeline.stage.pName = "main";
    const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s->pipeline);
    vkDestroyShaderModule(device, shader, nullptr);
    APEX_VK_TRY(result, "probe pipeline");
    return std::unique_ptr<ProbeVolume>(new ProbeVolume(std::move(s)));
}

Result<void> ProbeVolume::update(const DrawList& list, std::span<const GpuMaterial> materials) {
    auto& s = *impl_;
    const auto& settings = list.probeSettings;
    const std::uint64_t count = std::uint64_t(settings.dims.x) * settings.dims.y * settings.dims.z;
    if (glm::any(glm::lessThan(settings.dims, glm::uvec3(2))) || glm::any(glm::greaterThan(settings.dims, glm::uvec3(256))) || count > 65536 ||
        !std::isfinite(settings.spacing) || settings.spacing <= 0 || !settings.probesPerFrame ||
        !std::isfinite(settings.hysteresis) || settings.hysteresis < 0 || settings.hysteresis >= 1 ||
        !std::isfinite(settings.normalBias) || settings.normalBias < 0 ||
        (settings.cascades && (!std::isfinite(settings.cascadeScale) || settings.cascadeScale < 1 ||
            !std::isfinite(settings.cascadeFade) || settings.cascadeFade <= 0 ||
            !std::isfinite(settings.relocateRadius) || settings.relocateRadius <= 0 ||
            !std::isfinite(settings.relocateMax) || settings.relocateMax < settings.relocateRadius || settings.relocateMax > 2)))
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid realtime probe settings"});
    const Vec3 origin = Vec3(glm::floor(list.view.cameraPosition / double(settings.spacing)) * double(settings.spacing)) -
                        Vec3(settings.dims - 1u) * (settings.spacing * 0.5f);
    glm::uvec3 coarseDims{};
    Vec3 coarseOrigin{0};
    float coarseSpacing = settings.spacing;
    if (settings.cascades) {
        coarseDims = Impl::cascadeDims(settings.dims);
        if (std::uint64_t(coarseDims.x) * coarseDims.y * coarseDims.z > 65536)
            return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid realtime probe settings"});
        coarseSpacing = settings.spacing * settings.cascadeScale;
        coarseOrigin = Vec3(glm::floor(list.view.cameraPosition / double(coarseSpacing)) * double(coarseSpacing)) -
                       Vec3(coarseDims - 1u) * (coarseSpacing * 0.5f);
    }
    const auto& casters = list.shadowCasters ? *list.shadowCasters : list.meshes;
    auto change = s.scene.sync(casters, list.previews);
    if (!change) return std::unexpected(change.error());
    const bool materialChanged = materials.size() != s.lastMaterials.size() ||
        (!materials.empty() && std::memcmp(materials.data(), s.lastMaterials.data(), materials.size_bytes()) != 0);
    std::vector<std::uint64_t> surfaces;
    for (const auto& draw : casters) surfaces.push_back(std::uint64_t(draw.texture) | (std::uint64_t(draw.skySurface) << 32));
    if (*change != SceneAccelChange::Unchanged || materialChanged || surfaces != s.lastSurfaces) {
        std::vector<Triangle> triangles;
        std::vector<std::uint32_t> offsets;
        for (const auto& draw : casters) for (std::uint32_t instance = 0; instance < draw.instanceCount; ++instance) {
            DMat4 matrix(1);
            DVec3 drawOrigin = draw.origin;
            if (draw.previewIndex) {
                const auto& preview = list.previews[draw.previewIndex - 1 + (draw.instancedTransforms ? instance : 0)];
                matrix = preview.matrix;
                drawOrigin = preview.origin;
            }
            const auto transform = cameraRelativePreview(matrix, drawOrigin, DVec3(0));
            const auto world = [&](std::uint32_t vertex) {
                const auto& p = draw.cpuMesh->positions[vertex];
                const Vec4 v(p[0], p[1], p[2], 1);
                return Vec4(glm::dot(transform.rows[0], v), glm::dot(transform.rows[1], v),
                            glm::dot(transform.rows[2], v), 0);
            };
            offsets.push_back(static_cast<std::uint32_t>(triangles.size()));
            for (std::uint32_t i = draw.firstIndex; i < draw.firstIndex + draw.indexCount; i += 3) {
                const auto a = draw.cpuMesh->indices[i], b = draw.cpuMesh->indices[i + 1], c = draw.cpuMesh->indices[i + 2];
                const auto slot = draw.texture == kVertexMaterial && a < draw.cpuMesh->materials.size() ?
                    draw.cpuMesh->materials[a] : draw.texture;
                Vec4 albedo = slot < materials.size() ? materials[slot].bounce : Vec4(0.5f, 0.5f, 0.5f, 0);
                if (draw.skySurface) albedo.w = 1;
                triangles.push_back({world(a), world(b), world(c), albedo});
            }
        }
        if (!triangles.empty()) {
            if (auto result = upload(s.context, s.triangles, triangles.data(), triangles.size() * sizeof(Triangle)); !result) return result;
            if (auto result = upload(s.context, s.offsets, offsets.data(), offsets.size() * sizeof(std::uint32_t)); !result) return result;
        }
        s.lastMaterials.assign(materials.begin(), materials.end());
        s.lastSurfaces = std::move(surfaces);
        reset();
    }
    std::vector<GpuLight> lights;
    for (const auto& light : list.lights) lights.push_back(packLight(light, DVec3(0)));
    if (lights.size() != s.lastLights.size() || (!lights.empty() &&
        std::memcmp(lights.data(), s.lastLights.data(), lights.size() * sizeof(GpuLight)) != 0)) reset();
    // A non-null light address is retained even for an empty light list.
    GpuLight dummy{};
    if (auto result = upload(s.context, s.lights, lights.empty() ? &dummy : lights.data(),
                             std::max<std::size_t>(lights.size(), 1) * sizeof(GpuLight)); !result) return result;
    s.lastLights = std::move(lights);
    if (s.settings != settings || s.origin != origin || s.coarseDims != coarseDims || s.coarseOrigin != coarseOrigin) reset();
    s.settings = settings; s.origin = origin; s.coarseDims = coarseDims; s.coarseOrigin = coarseOrigin;
    const auto size = count * sizeof(GpuIrradianceProbe);
    for (auto& maps : s.maps) if (maps.size() != size) {
        auto created = Buffer::create(s.context, {size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly});
        if (!created) return std::unexpected(created.error());
        maps = std::move(*created);
        reset();
    }
    const auto coarseSize = coarseDims.x * coarseDims.y * coarseDims.z * sizeof(GpuIrradianceProbe);
    for (auto& maps : s.coarseMaps) if (maps.size() != coarseSize) {
        if (coarseSize == 0) { maps = {}; continue; }
        auto created = Buffer::create(s.context, {coarseSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly});
        if (!created) return std::unexpected(created.error());
        maps = std::move(*created);
        reset();
    }
    const auto tlas = s.scene.tlas();
    s.ready = tlas != VK_NULL_HANDLE;
    if (s.ready) {
        auto acceleration = vkStruct<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
        acceleration.accelerationStructureCount = 1; acceleration.pAccelerationStructures = &tlas;
        auto write = vkStruct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        write.pNext = &acceleration; write.dstSet = s.set; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(s.context.device(), 1, &write, 0, nullptr);
        // The addresses recorded here are the maps record() will read as "previous".
        CascadeContext context;
        context.fineDims = glm::uvec4(settings.dims, 0);
        context.fineOriginSpacing = Vec4(origin, settings.spacing);
        context.fineParams = Vec4(settings.normalBias, settings.cascades ? settings.cascadeFade : 0.0f, 0, 0);
        context.addresses = glm::uvec4(std::uint32_t(s.maps[s.current].deviceAddress()),
                                       std::uint32_t(s.maps[s.current].deviceAddress() >> 32),
                                       s.coarseDims.x && s.coarseMaps[s.current].size() ?
                                           std::uint32_t(s.coarseMaps[s.current].deviceAddress()) : 0,
                                       s.coarseDims.x && s.coarseMaps[s.current].size() ?
                                           std::uint32_t(s.coarseMaps[s.current].deviceAddress() >> 32) : 0);
        context.coarseDims = glm::uvec4(coarseDims, 0);
        context.coarseOriginSpacing = Vec4(coarseOrigin, coarseSpacing);
        context.coarseParams = Vec4(s.cascadeBias(), settings.relocate ? 1.0f : 0.0f,
                                    settings.relocateRadius, settings.relocateMax);
        if (auto result = upload(s.context, s.cascadeContext, &context, sizeof(context)); !result) return result;
    }
    return {};
}
void ProbeVolume::reset() {
    auto& s = *impl_;
    s.clear = true; s.cursor = 0; s.refresh = 0; s.coarseCursor = 0; s.coarseRefresh = 0;
}
VkDeviceAddress ProbeVolume::address() const {
    auto& s = *impl_;
    return s.ready ? s.maps[s.current].deviceAddress() : 0;
}
VkBuffer ProbeVolume::buffer() const {
    auto& s = *impl_;
    return s.ready ? s.maps[s.current].handle() : VK_NULL_HANDLE;
}
VkBuffer ProbeVolume::coarseBuffer() const {
    auto& s = *impl_;
    return s.ready ? s.coarseMaps[s.current].handle() : VK_NULL_HANDLE;
}
void ProbeVolume::pack(FrameBlock& frame, DVec3 camera) const {
    const auto& s = *impl_;
    if (!s.ready) return;
    frame.indirectProbes = address();
    frame.indirectDims = glm::uvec4(s.settings.dims, 0);
    frame.indirectOrigin = Vec4(s.origin - Vec3(camera), 0);
    frame.indirectParams = Vec4(s.settings.spacing, s.settings.normalBias,
        s.settings.cascades ? s.settings.cascadeFade : 0.0f, 0);
    frame.indirectCoarse = 0; frame.indirectCoarseDims = glm::uvec4(0);
    frame.indirectCoarseOrigin = Vec4(0); frame.indirectCoarseParams = Vec4(0);
    if (s.coarseDims.x && s.coarseMaps[s.current].size()) {
        frame.indirectCoarse = s.coarseMaps[s.current].deviceAddress();
        frame.indirectCoarseDims = glm::uvec4(s.coarseDims, 0);
        frame.indirectCoarseOrigin = Vec4(s.coarseOrigin - Vec3(camera), 0);
        frame.indirectCoarseParams = Vec4(s.cascadeSpacing(), s.cascadeBias(), 0, 0);
    }
}
void ProbeVolume::record(VkCommandBuffer commands) {
    auto& s = *impl_;
    if (!s.ready) return;
    barrier(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const auto previous = s.current;
    s.current ^= 1;
    if (s.clear) {
        vkCmdFillBuffer(commands, s.maps[previous].handle(), 0, VK_WHOLE_SIZE, 0);
        if (s.coarseMaps[0].size()) vkCmdFillBuffer(commands, s.coarseMaps[previous].handle(), 0, VK_WHOLE_SIZE, 0);
        barrier(commands, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        s.clear = false;
    }
    const VkBufferCopy copy{0, 0, s.maps[previous].size()};
    vkCmdCopyBuffer(commands, s.maps[previous].handle(), s.maps[s.current].handle(), 1, &copy);
    if (s.coarseMaps[0].size()) {
        const VkBufferCopy coarseCopy{0, 0, s.coarseMaps[previous].size()};
        vkCmdCopyBuffer(commands, s.coarseMaps[previous].handle(), s.coarseMaps[s.current].handle(), 1, &coarseCopy);
    }
    barrier(commands, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &s.set, 0, nullptr);
    const auto advance = [](std::uint32_t& cursor, std::uint32_t& refresh, std::uint32_t count, std::uint32_t budget) {
        refresh += (cursor + budget) / count;
        cursor = (cursor + budget) % count;
    };
    const auto dispatch = [&](const Buffer& previousMaps, const Buffer& output, glm::uvec4 dims, Vec4 originSpacing,
                              Vec4 params, std::uint32_t count, std::uint32_t first, std::uint32_t budget) {
        const UpdatePush push{previousMaps.deviceAddress(), output.deviceAddress(), s.triangles.deviceAddress(),
            s.offsets.deviceAddress(), s.lights.deviceAddress(), static_cast<std::uint32_t>(s.lastLights.size()), first,
            dims, originSpacing, params, s.cascadeContext.deviceAddress()};
        vkCmdPushConstants(commands, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(commands, budget, 1, 1);
    };
    // Fine cascade, then the coarser one, each with its own budget and cursor.
    const auto fineCount = s.settings.dims.x * s.settings.dims.y * s.settings.dims.z;
    const auto fineBudget = std::min({fineCount, s.settings.probesPerFrame, 65535u});
    dispatch(s.maps[previous], s.maps[s.current], glm::uvec4(s.settings.dims, 0), Vec4(s.origin, s.settings.spacing),
             Vec4(s.settings.hysteresis, s.settings.normalBias, float(s.refresh % 128) * 0.754877666f, s.settings.spacing * 4.0f),
             fineCount, s.cursor, fineBudget);
    if (s.coarseDims.x && s.coarseMaps[0].size()) {
        const auto coarseCount = s.coarseDims.x * s.coarseDims.y * s.coarseDims.z;
        // probesPerFrame budgets the fine cascade; the coarse one gets the same fraction of its
        // own probes, so both cascades complete a refresh in the same number of frames.
        const auto coarseBudget = std::min({coarseCount, 65535u, std::max(1u,
            std::uint32_t(std::uint64_t(fineBudget) * coarseCount / fineCount))});
        dispatch(s.coarseMaps[previous], s.coarseMaps[s.current], glm::uvec4(s.coarseDims, 0),
                 Vec4(s.coarseOrigin, s.cascadeSpacing()),
                 Vec4(s.settings.hysteresis, s.cascadeBias(), float(s.coarseRefresh % 128) * 0.754877666f,
                      s.cascadeSpacing() * 4.0f),
                 coarseCount, s.coarseCursor, coarseBudget);
        advance(s.coarseCursor, s.coarseRefresh, coarseCount, coarseBudget);
    }
    barrier(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);
    advance(s.cursor, s.refresh, fineCount, fineBudget);
}
} // namespace apex::render
