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
};
static_assert(sizeof(UpdatePush) == 96);

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
    Buffer maps[2], triangles, offsets, lights;
    std::vector<GpuLight> lastLights;
    std::vector<GpuMaterial> lastMaterials;
    std::vector<std::uint64_t> lastSurfaces;
    ProbeSettings settings;
    Vec3 origin{0};
    std::uint32_t current = 0, cursor = 0, refresh = 0;
    bool clear = true, ready = false;
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
        !std::isfinite(settings.normalBias) || settings.normalBias < 0)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid realtime probe settings"});
    const Vec3 origin = Vec3(glm::floor(list.view.cameraPosition / double(settings.spacing)) * double(settings.spacing)) -
                        Vec3(settings.dims - 1u) * (settings.spacing * 0.5f);
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
    if (s.settings != settings || s.origin != origin) reset();
    s.settings = settings; s.origin = origin;
    const auto size = count * sizeof(GpuIrradianceProbe);
    for (auto& maps : s.maps) if (maps.size() != size) {
        auto created = Buffer::create(s.context, {size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
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
    }
    return {};
}
void ProbeVolume::reset() { impl_->clear = true; impl_->cursor = 0; impl_->refresh = 0; }
VkDeviceAddress ProbeVolume::address() const { return impl_->ready ? impl_->maps[impl_->current].deviceAddress() : 0; }
void ProbeVolume::pack(FrameBlock& frame, DVec3 camera) const {
    const auto& s = *impl_;
    if (!s.ready) return;
    frame.indirectProbes = address();
    frame.indirectDims = glm::uvec4(s.settings.dims, 0);
    frame.indirectOrigin = Vec4(s.origin - Vec3(camera), 0);
    frame.indirectParams = Vec4(s.settings.spacing, s.settings.normalBias, 0, 0);
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
        barrier(commands, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        s.clear = false;
    }
    const VkBufferCopy copy{0, 0, s.maps[previous].size()};
    vkCmdCopyBuffer(commands, s.maps[previous].handle(), s.maps[s.current].handle(), 1, &copy);
    barrier(commands, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    const auto count = s.settings.dims.x * s.settings.dims.y * s.settings.dims.z;
    const auto budget = std::min({count, s.settings.probesPerFrame, 65535u});
    const UpdatePush push{s.maps[previous].deviceAddress(), address(), s.triangles.deviceAddress(),
        s.offsets.deviceAddress(), s.lights.deviceAddress(), static_cast<std::uint32_t>(s.lastLights.size()), s.cursor,
        glm::uvec4(s.settings.dims, 0), Vec4(s.origin, s.settings.spacing),
        Vec4(s.settings.hysteresis, s.settings.normalBias, float(s.refresh % 128) * 0.754877666f, 262144.0f)};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &s.set, 0, nullptr);
    vkCmdPushConstants(commands, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commands, budget, 1, 1);
    barrier(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);
    s.refresh += (s.cursor + budget) / count;
    s.cursor = (s.cursor + budget) % count;
}
} // namespace apex::render
