#include "render/gpu_bake.hpp"

#include "render/accel.hpp"
#include "render/resources.hpp"
#include "render/shader_blobs.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace apex::render {
namespace {

struct GpuFace {
    Vec4 normal;
    Vec4 albedo;
    Vec4 chartOrigin;
    Vec4 luxelToWorld[2];
    Vec4 worldToLuxel[2];
    Vec4 offsetMins;
    glm::uvec4 dimensionsVertices;
    glm::uvec4 source;
};
static_assert(sizeof(GpuFace) == 160);

struct GpuLight {
    Vec4 originRadius;
    Vec4 intensity;
    Vec4 directionStopdot;
    Vec4 shadowOffsetStopdot2;
    Vec4 attenuationExponent;
    glm::uvec4 kindDirect;
};
static_assert(sizeof(GpuLight) == 96);

struct GpuWork { glm::uvec4 value; };
struct GpuPrimitive { glm::uvec4 value; };

struct BakePush {
    VkDeviceAddress faces, vertices, lights, primitives, work, source, output;
    std::uint32_t first, workCount, lightCount, skyRays;
    std::uint32_t bounceRays, level, seedLow, seedHigh;
    std::uint32_t hasGeometry;
    float bounceReflectivity;
};
static_assert(sizeof(BakePush) == 96);

Result<void> upload(Context& context, Buffer& buffer, const void* data, std::size_t size) {
    const auto allocated = std::max<std::size_t>(size, 16);
    if (buffer.size() < allocated) {
        auto created = Buffer::create(context, {allocated, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
        if (!created) return std::unexpected(created.error());
        buffer = std::move(*created);
    }
    if (size) std::memcpy(buffer.mapped(), data, size);
    buffer.flush();
    return {};
}

Result<void> readback(Context& context, Buffer& buffer, std::size_t size) {
    const auto allocated = std::max<std::size_t>(size, 16);
    if (buffer.size() < allocated) {
        auto created = Buffer::create(context, {allocated, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Readback});
        if (!created) return std::unexpected(created.error());
        buffer = std::move(*created);
    }
    return {};
}

[[noreturn]] void fail(const Error& error) {
    throw std::runtime_error(std::string("GPU bake: ") + error.what());
}

} // namespace

struct GpuBakeKernel::Impl {
    Impl(Context& context, lighting::BakerOptions options) : context(context), options(options) {}
    ~Impl() {
        if (pipeline) vkDestroyPipeline(context.device(), pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(context.device(), layout, nullptr);
        if (pool) vkDestroyDescriptorPool(context.device(), pool, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(context.device(), setLayout, nullptr);
        if (fence) vkDestroyFence(context.device(), fence, nullptr);
        if (commandPool) vkDestroyCommandPool(context.device(), commandPool, nullptr);
    }

    Context& context;
    lighting::BakerOptions options;
    std::unique_ptr<Accel> blas, tlas;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer faces, vertices, lights, primitives, work, output;
    // One snapshot per bounce level: a budget-limited traversal may resume after a deeper level has started.
    std::vector<Buffer> sources;
    std::size_t sourceValues = 0, lightCount = 0;
    bool hasGeometry = false;
};

GpuBakeKernel::GpuBakeKernel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuBakeKernel::~GpuBakeKernel() = default;

Result<std::shared_ptr<GpuBakeKernel>> GpuBakeKernel::create(
    Context& context, const lighting::BakeScene& scene, lighting::BakerOptions options) {
    if (!context.info().rayQuery)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "GPU bake requires ray query"});
    auto impl = std::make_unique<Impl>(context, options);
    impl->sources.resize(static_cast<std::size_t>(options.bounces) + 1);

    std::vector<GpuFace> faces;
    std::vector<Vec4> vertices;
    std::uint32_t sourceOffset = 0;
    for (const auto& face : scene.faces) {
        const auto luxels = std::uint64_t(face.chart.dimensions[0]) * face.chart.dimensions[1];
        if (face.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
            vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
            luxels > std::numeric_limits<std::uint32_t>::max() - sourceOffset)
            return std::unexpected(Error{ErrorCode::InvalidArgument, "GPU bake scene is too large"});
        const auto firstVertex = static_cast<std::uint32_t>(vertices.size());
        for (const auto& vertex : face.vertices) vertices.emplace_back(Vec3(vertex), 0);
        GpuFace gpu{};
        gpu.normal = Vec4(Vec3(face.normal), 0);
        gpu.albedo = Vec4(face.albedo, 0);
        gpu.chartOrigin = Vec4(Vec3(face.chart.origin), 0);
        for (int axis = 0; axis < 2; ++axis) {
            gpu.luxelToWorld[axis] = Vec4(Vec3(face.chart.luxelToWorld[axis]), 0);
            gpu.worldToLuxel[axis] = Vec4(Vec3(face.chart.worldToLuxel[axis]), 0);
        }
        gpu.offsetMins = Vec4(Vec2(face.chart.offset), Vec2(face.chart.mins[0], face.chart.mins[1]));
        gpu.dimensionsVertices = {static_cast<std::uint32_t>(face.chart.dimensions[0]),
            static_cast<std::uint32_t>(face.chart.dimensions[1]), firstVertex,
            static_cast<std::uint32_t>(face.vertices.size())};
        gpu.source = {sourceOffset, 0, 0, 0};
        sourceOffset += static_cast<std::uint32_t>(luxels);
        faces.push_back(gpu);
    }
    impl->sourceValues = sourceOffset;

    std::vector<GpuLight> lights;
    for (const auto& light : scene.lights) {
        GpuLight gpu{};
        gpu.originRadius = Vec4(Vec3(light.origin), static_cast<float>(light.radius));
        gpu.intensity = Vec4(Vec3(light.intensity), 0);
        gpu.directionStopdot = Vec4(Vec3(light.direction), static_cast<float>(light.stopdot));
        gpu.shadowOffsetStopdot2 = Vec4(Vec3(light.shadowCastOffset), static_cast<float>(light.stopdot2));
        gpu.attenuationExponent = Vec4(light.constant, light.linear, light.quadratic, light.exponent);
        gpu.kindDirect = {static_cast<std::uint32_t>(light.kind),
                          static_cast<std::uint32_t>(light.directMode), 0, 0};
        lights.push_back(gpu);
    }
    impl->lightCount = lights.size();

    std::vector<GpuPrimitive> primitives;
    lighting::TriangleMesh dummy;
    const lighting::TriangleMesh* mesh = nullptr;
    if (scene.occlusion.geometry && !scene.occlusion.geometry->mesh.indices.empty()) {
        mesh = &scene.occlusion.geometry->mesh;
        impl->hasGeometry = true;
        for (const auto& primitive : scene.occlusion.geometry->primitives) {
            if (primitive.face && *primitive.face >= scene.faces.size())
                return std::unexpected(Error{ErrorCode::InvalidArgument, "GPU bake caster has an invalid face index"});
            primitives.push_back({{primitive.sky ? 1u : 0u,
                primitive.face ? static_cast<std::uint32_t>(*primitive.face) : std::numeric_limits<std::uint32_t>::max(), 0, 0}});
        }
    } else {
        dummy.positions = {{-1, -1, -1e20f}, {1, -1, -1e20f}, {0, 1, -1e20f}};
        dummy.indices = {0, 1, 2};
        mesh = &dummy;
        primitives.push_back({{1, std::numeric_limits<std::uint32_t>::max(), 0, 0}});
    }
    if (primitives.size() != mesh->indices.size() / 3)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "GPU bake caster table does not match its mesh"});
    auto blas = Accel::buildBlas(context, mesh->positions, mesh->indices);
    if (!blas) return std::unexpected(blas.error());
    impl->blas = std::move(*blas);
    const std::array<AccelInstance, 1> instances{{{impl->blas.get()}}};
    auto tlas = Accel::buildTlas(context, instances);
    if (!tlas) return std::unexpected(tlas.error());
    impl->tlas = std::move(*tlas);

    if (auto result = upload(context, impl->faces, faces.data(), faces.size() * sizeof(GpuFace)); !result) return std::unexpected(result.error());
    if (auto result = upload(context, impl->vertices, vertices.data(), vertices.size() * sizeof(Vec4)); !result) return std::unexpected(result.error());
    if (auto result = upload(context, impl->lights, lights.data(), lights.size() * sizeof(GpuLight)); !result) return std::unexpected(result.error());
    if (auto result = upload(context, impl->primitives, primitives.data(), primitives.size() * sizeof(GpuPrimitive)); !result) return std::unexpected(result.error());

    const auto device = context.device();
    auto commandPool = vkStruct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    commandPool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    commandPool.queueFamilyIndex = context.graphicsFamily();
    APEX_VK_TRY(vkCreateCommandPool(device, &commandPool, nullptr, &impl->commandPool), "GPU bake command pool");
    auto allocateCommand = vkStruct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocateCommand.commandPool = impl->commandPool;
    allocateCommand.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateCommand.commandBufferCount = 1;
    APEX_VK_TRY(vkAllocateCommandBuffers(device, &allocateCommand, &impl->commands), "GPU bake command buffer");
    auto fence = vkStruct<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    APEX_VK_TRY(vkCreateFence(device, &fence, nullptr, &impl->fence), "GPU bake fence");

    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                                               VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    auto setInfo = vkStruct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    setInfo.bindingCount = 1; setInfo.pBindings = &binding;
    APEX_VK_TRY(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &impl->setLayout), "GPU bake set layout");
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
    auto poolInfo = vkStruct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    poolInfo.maxSets = 1; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
    APEX_VK_TRY(vkCreateDescriptorPool(device, &poolInfo, nullptr, &impl->pool), "GPU bake descriptor pool");
    auto allocateSet = vkStruct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocateSet.descriptorPool = impl->pool; allocateSet.descriptorSetCount = 1; allocateSet.pSetLayouts = &impl->setLayout;
    APEX_VK_TRY(vkAllocateDescriptorSets(device, &allocateSet, &impl->set), "GPU bake descriptor set");
    const auto handle = impl->tlas->handle();
    auto acceleration = vkStruct<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
    acceleration.accelerationStructureCount = 1; acceleration.pAccelerationStructures = &handle;
    auto write = vkStruct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.pNext = &acceleration; write.dstSet = impl->set; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BakePush)};
    auto layoutInfo = vkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &impl->setLayout;
    layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &range;
    APEX_VK_TRY(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &impl->layout), "GPU bake pipeline layout");
    const auto code = builtinSpirv("bake_comp");
    if (code.empty()) return std::unexpected(Error{ErrorCode::Internal, "GPU bake shader is missing"});
    auto moduleInfo = vkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    moduleInfo.codeSize = code.size_bytes(); moduleInfo.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    APEX_VK_TRY(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "GPU bake shader");
    auto pipelineInfo = vkStruct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipelineInfo.layout = impl->layout;
    pipelineInfo.stage = vkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipelineInfo.stage.module = module; pipelineInfo.stage.pName = "main";
    const auto pipelineResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &impl->pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    APEX_VK_TRY(pipelineResult, "GPU bake pipeline");
    return std::shared_ptr<GpuBakeKernel>(new GpuBakeKernel(std::move(impl)));
}

void GpuBakeKernel::run(unsigned level, std::span<const lighting::BakeWork> work,
                        const std::vector<std::vector<Vec3>>* source, bool sourceChanged,
                        std::span<Vec3> sums) {
    auto& impl = *impl_;
    if (sums.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("GPU bake: work is too large");
    std::vector<GpuWork> flattened;
    flattened.reserve(sums.size());
    for (const auto& item : work) for (auto luxel = item.begin; luxel < item.end; ++luxel) {
        const auto count = item.samples[luxel];
        flattened.push_back({{static_cast<std::uint32_t>(item.face), static_cast<std::uint32_t>(luxel),
                              static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(count >> 32)}});
    }
    if (flattened.size() != sums.size()) throw std::invalid_argument("GPU bake: output size does not match work");
    if (flattened.empty()) return;
    if (auto result = upload(impl.context, impl.work, flattened.data(), flattened.size() * sizeof(GpuWork)); !result) fail(result.error());
    if (auto result = readback(impl.context, impl.output, flattened.size() * sizeof(Vec4)); !result) fail(result.error());
    if (level > 0 && sourceChanged) {
        if (!source) throw std::invalid_argument("GPU bake: bounce source is missing");
        std::vector<Vec4> values;
        values.reserve(impl.sourceValues);
        for (const auto& face : *source) for (const auto value : face) values.emplace_back(value, 0);
        if (values.size() != impl.sourceValues) throw std::invalid_argument("GPU bake: bounce source size is invalid");
        if (auto result = upload(impl.context, impl.sources.at(level), values.data(), values.size() * sizeof(Vec4)); !result) fail(result.error());
    }

    BakePush push{impl.faces.deviceAddress(), impl.vertices.deviceAddress(), impl.lights.deviceAddress(),
        impl.primitives.deviceAddress(), impl.work.deviceAddress(), impl.sources.at(level).deviceAddress(), impl.output.deviceAddress(),
        0, static_cast<std::uint32_t>(flattened.size()), 0, impl.options.skyRays, impl.options.bounceRays, level,
        static_cast<std::uint32_t>(impl.options.seed), static_cast<std::uint32_t>(impl.options.seed >> 32),
        impl.hasGeometry ? 1u : 0u, static_cast<float>(impl.options.bounceReflectivity)};
    push.lightCount = static_cast<std::uint32_t>(impl.lightCount);

    vkResetCommandPool(impl.context.device(), impl.commandPool, 0);
    auto begin = vkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (const auto result = vkBeginCommandBuffer(impl.commands, &begin); result != VK_SUCCESS) fail(vkError(result, "GPU bake begin"));
    vkCmdBindPipeline(impl.commands, VK_PIPELINE_BIND_POINT_COMPUTE, impl.pipeline);
    vkCmdBindDescriptorSets(impl.commands, VK_PIPELINE_BIND_POINT_COMPUTE, impl.layout, 0, 1, &impl.set, 0, nullptr);
    constexpr std::uint32_t maxInvocations = 65535u * 64u;
    for (std::uint32_t first = 0; first < push.workCount; first += maxInvocations) {
        push.first = first;
        vkCmdPushConstants(impl.commands, impl.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        const auto count = std::min(maxInvocations, push.workCount - first);
        vkCmdDispatch(impl.commands, (count + 63) / 64, 1, 1);
    }
    auto barrier = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    auto dependency = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(impl.commands, &dependency);
    if (const auto result = vkEndCommandBuffer(impl.commands); result != VK_SUCCESS) fail(vkError(result, "GPU bake end"));
    auto command = vkStruct<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    command.commandBuffer = impl.commands;
    auto submit = vkStruct<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
    submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &command;
    {
        auto lock = impl.context.lockQueue();
        if (const auto result = vkQueueSubmit2(impl.context.graphicsQueue(), 1, &submit, impl.fence); result != VK_SUCCESS)
            fail(vkError(result, "GPU bake submit"));
    }
    const auto waited = vkWaitForFences(impl.context.device(), 1, &impl.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(impl.context.device(), 1, &impl.fence);
    if (waited != VK_SUCCESS) fail(vkError(waited, "GPU bake wait"));
    impl.output.invalidate();
    const auto* output = static_cast<const Vec4*>(impl.output.mapped());
    for (std::size_t i = 0; i < sums.size(); ++i) sums[i] = Vec3(output[i]);
}

} // namespace apex::render
