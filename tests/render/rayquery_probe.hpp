#pragma once

#include "render/resources.hpp"
#include "render/test_shaders_spv.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <span>
#include <vector>

namespace apex::render::testing {

struct Ray {
    std::array<float, 4> origin;
    std::array<float, 4> direction{0, 0, -1, 100};
    std::array<std::uint32_t, 4> options{0xFF, 0, 0, 0};
};
struct Hit {
    std::uint32_t hit;
    float distance;
    std::uint32_t customIndex;
    std::uint32_t primitive;
};
static_assert(sizeof(Ray) == 48 && sizeof(Hit) == 16);

struct Probe {
    VkDevice device;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ~Probe() {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
};

inline std::vector<Hit> traceRays(Context* context, VkAccelerationStructureKHR tlas, std::span<const Ray> rays) {
    auto input = Buffer::create(*context, {rays.size_bytes(),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
    auto output = Buffer::create(*context, {rays.size() * sizeof(Hit),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Readback});
    REQUIRE(input);
    REQUIRE(output);
    std::memcpy(input->mapped(), rays.data(), rays.size_bytes());
    input->flush();

    Probe probe{context->device()};
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                                               VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    auto setLayout = vkStruct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    setLayout.bindingCount = 1; setLayout.pBindings = &binding;
    REQUIRE(vkCreateDescriptorSetLayout(probe.device, &setLayout, nullptr, &probe.setLayout) == VK_SUCCESS);
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
    auto pool = vkStruct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &poolSize;
    REQUIRE(vkCreateDescriptorPool(probe.device, &pool, nullptr, &probe.pool) == VK_SUCCESS);
    auto allocate = vkStruct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = probe.pool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &probe.setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    REQUIRE(vkAllocateDescriptorSets(probe.device, &allocate, &set) == VK_SUCCESS);
    const auto handle = tlas;
    auto acceleration = vkStruct<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
    acceleration.accelerationStructureCount = 1; acceleration.pAccelerationStructures = &handle;
    auto write = vkStruct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.pNext = &acceleration; write.dstSet = set; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(probe.device, 1, &write, 0, nullptr);
    auto shader = vkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    shader.codeSize = sizeof(shaders::rayquery_probe_comp); shader.pCode = shaders::rayquery_probe_comp;
    REQUIRE(vkCreateShaderModule(probe.device, &shader, nullptr, &probe.shader) == VK_SUCCESS);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(VkDeviceAddress)};
    auto layout = vkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.setLayoutCount = 1; layout.pSetLayouts = &probe.setLayout;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
    REQUIRE(vkCreatePipelineLayout(probe.device, &layout, nullptr, &probe.layout) == VK_SUCCESS);
    auto pipeline = vkStruct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipeline.layout = probe.layout;
    pipeline.stage = vkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = probe.shader; pipeline.stage.pName = "main";
    REQUIRE(vkCreateComputePipelines(probe.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &probe.pipeline) == VK_SUCCESS);

    const auto trace = [&] {
        REQUIRE(context->oneShot([&](VkCommandBuffer commands) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe.pipeline);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe.layout, 0, 1, &set, 0, nullptr);
            const std::array<VkDeviceAddress, 2> addresses{input->deviceAddress(), output->deviceAddress()};
            vkCmdPushConstants(commands, probe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(addresses), addresses.data());
            vkCmdDispatch(commands, static_cast<std::uint32_t>(rays.size()), 1, 1);
            auto barrier = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            auto dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
            dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commands, &dep);
        }));
        output->invalidate();
    };
    trace();
    const auto* hits = static_cast<const Hit*>(output->mapped());
    return {hits, hits + rays.size()};
}

} // namespace apex::render::testing
