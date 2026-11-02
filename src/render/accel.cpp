#include "render/accel.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace apex::render {

namespace {

constexpr VkBufferUsageFlags kBuildInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

Result<Buffer> upload(Context& context, std::span<const std::byte> bytes) {
    auto buffer = Buffer::create(context, {bytes.size(), kBuildInput, MemoryUsage::Upload});
    if (!buffer) return std::unexpected(buffer.error());
    std::memcpy(buffer->mapped(), bytes.data(), bytes.size());
    buffer->flush();
    return buffer;
}

VkPhysicalDeviceAccelerationStructurePropertiesKHR properties(Context& context) {
    auto acceleration = vkStruct<VkPhysicalDeviceAccelerationStructurePropertiesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR);
    auto props = vkStruct<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    props.pNext = &acceleration;
    vkGetPhysicalDeviceProperties2(context.physicalDevice(), &props);
    return acceleration;
}

} // namespace

Accel::~Accel() {
    if (handle_) vkDestroyAccelerationStructureKHR(context_.device(), handle_, nullptr);
}

Result<std::unique_ptr<Accel>> Accel::buildBlas(Context& context,
    std::span<const std::array<float, 3>> positions, std::span<const std::uint32_t> indices) {
    if (!context.info().accelerationStructure) {
        return std::unexpected(Error{ErrorCode::Unsupported, "acceleration structures are not enabled"});
    }
    if (positions.empty() || positions.size() > std::numeric_limits<std::uint32_t>::max() ||
        indices.empty() || indices.size() % 3 != 0 ||
        indices.size() / 3 > properties(context).maxPrimitiveCount ||
        indices.size() / 3 > std::numeric_limits<std::uint32_t>::max() ||
        std::ranges::any_of(indices, [&](auto index) { return index >= positions.size(); })) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid BLAS triangle list"});
    }
    auto vertices = upload(context, std::as_bytes(positions));
    if (!vertices) return std::unexpected(vertices.error());
    auto elements = upload(context, std::as_bytes(indices));
    if (!elements) return std::unexpected(elements.error());

    auto geometry = vkStruct<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& triangles = geometry.geometry.triangles;
    triangles = vkStruct<VkAccelerationStructureGeometryTrianglesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertices->deviceAddress();
    triangles.vertexStride = sizeof(positions[0]);
    triangles.maxVertex = static_cast<std::uint32_t>(positions.size() - 1);
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = elements->deviceAddress();
    std::unique_ptr<Accel> result(new Accel(context, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR));
    auto built = result->build(geometry, static_cast<std::uint32_t>(indices.size() / 3), false);
    if (!built) return std::unexpected(built.error());
    return result;
}

Result<std::unique_ptr<Accel>> Accel::buildTlas(Context& context, std::span<const AccelInstance> instances) {
    if (!context.info().accelerationStructure) {
        return std::unexpected(Error{ErrorCode::Unsupported, "acceleration structures are not enabled"});
    }
    std::unique_ptr<Accel> result(new Accel(context, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR));
    auto built = result->buildInstances(instances, false);
    if (!built) return std::unexpected(built.error());
    return result;
}

Result<void> Accel::updateInstances(std::span<const AccelInstance> instances) {
    if (type_ != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR || instances.size() != instanceCount_) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "TLAS update requires the original instance count"});
    }
    return buildInstances(instances, true);
}

Result<void> Accel::buildInstances(std::span<const AccelInstance> instances, bool update) {
    if (instances.empty() || instances.size() > properties(context_).maxInstanceCount ||
        instances.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid TLAS instance count"});
    }
    std::vector<VkAccelerationStructureInstanceKHR> packed;
    packed.reserve(instances.size());
    for (const auto& instance : instances) {
        if (!instance.blas || &instance.blas->context_ != &context_ ||
            instance.blas->type_ != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR ||
            instance.customIndex > 0xFFFFFFu) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid TLAS BLAS reference or custom index"});
        }
        VkAccelerationStructureInstanceKHR value{};
        value.transform = instance.transform;
        value.instanceCustomIndex = instance.customIndex;
        value.mask = instance.mask;
        value.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        value.accelerationStructureReference = instance.blas->deviceAddress();
        packed.push_back(value);
    }
    auto input = upload(context_, std::as_bytes(std::span(packed)));
    if (!input) return std::unexpected(input.error());
    auto geometry = vkStruct<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = vkStruct<VkAccelerationStructureGeometryInstancesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
    geometry.geometry.instances.data.deviceAddress = input->deviceAddress();
    const auto count = static_cast<std::uint32_t>(instances.size());
    auto built = build(geometry, count, update);
    if (!built) return built;
    instanceCount_ = count;
    return {};
}

Result<void> Accel::build(const VkAccelerationStructureGeometryKHR& geometry, std::uint32_t count, bool update) {
    auto info = vkStruct<VkAccelerationStructureBuildGeometryInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR);
    info.type = type_;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (type_ == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR) info.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    auto sizes = vkStruct<VkAccelerationStructureBuildSizesInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR);
    vkGetAccelerationStructureBuildSizesKHR(context_.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                          &info, &count, &sizes);
    if (!update) {
        auto storage = Buffer::create(context_, {sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            MemoryUsage::GpuOnly});
        if (!storage) return std::unexpected(storage.error());
        storage_ = std::move(*storage);
        auto create = vkStruct<VkAccelerationStructureCreateInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR);
        create.buffer = storage_.handle();
        create.size = sizes.accelerationStructureSize;
        create.type = type_;
        APEX_VK_TRY(vkCreateAccelerationStructureKHR(context_.device(), &create, nullptr, &handle_), "vkCreateAccelerationStructureKHR");
        auto address = vkStruct<VkAccelerationStructureDeviceAddressInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR);
        address.accelerationStructure = handle_;
        address_ = vkGetAccelerationStructureDeviceAddressKHR(context_.device(), &address);
    }
    const VkDeviceSize alignment = properties(context_).minAccelerationStructureScratchOffsetAlignment;
    auto scratch = Buffer::create(context_, {(update ? sizes.updateScratchSize : sizes.buildScratchSize) + alignment,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::GpuOnly});
    if (!scratch) return std::unexpected(scratch.error());
    info.scratchData.deviceAddress = (scratch->deviceAddress() + alignment - 1) & ~(alignment - 1);
    info.srcAccelerationStructure = update ? handle_ : VK_NULL_HANDLE;
    info.dstAccelerationStructure = handle_;
    const VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    const auto* ranges = &range;
    return context_.oneShot([&](VkCommandBuffer commands) {
        // Order prior builds/queries before an update, and make BLAS writes visible to TLAS builds.
        auto barrier = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        auto dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dep);
        vkCmdBuildAccelerationStructuresKHR(commands, 1, &info, &ranges);
        // Subsequent builds and shader queries on this queue can consume the result directly.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier2(commands, &dep);
    });
}

} // namespace apex::render
