#include "render/scene_target.hpp"
#include "render/test_shaders_spv.hpp"
#include "shared_context.hpp"

#include <doctest/doctest.h>

#include <array>

using namespace apex;
using namespace apex::render;

namespace {

struct Probe {
    VkDevice device;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ~Probe() {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
    }
};

} // namespace

TEST_CASE("render: sampled depth comparison uses reversed-Z and compute floats round-trip") {
    Context* context = testing::sharedContext();
    if (!context) {
        MESSAGE("Skipped: no Vulkan 1.3 device");
        return;
    }
    context->validation().clear();
    auto heap = TextureHeap::create(*context, {8, 1u << 20});
    REQUIRE(heap);
    SceneTarget scene(*context, **heap);
    REQUIRE(scene.resize(8, 8));
    auto output = Buffer::create(*context, {5 * sizeof(float),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Readback});
    REQUIRE(output);

    Probe probe{context->device()};
    VkShaderModuleCreateInfo shader = vkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    shader.codeSize = sizeof(shaders::attachment_probe_comp);
    shader.pCode = shaders::attachment_probe_comp;
    REQUIRE(vkCreateShaderModule(probe.device, &shader, nullptr, &probe.shader) == VK_SUCCESS);
    // A test-only compute layout; the renderer's 128-byte push/preview ABI is untouched.
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 3 * sizeof(std::uint32_t)};
    const VkDescriptorSetLayout setLayout = (*heap)->layout();
    VkPipelineLayoutCreateInfo layout = vkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &setLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    REQUIRE(vkCreatePipelineLayout(probe.device, &layout, nullptr, &probe.layout) == VK_SUCCESS);
    VkComputePipelineCreateInfo pipeline = vkStruct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipeline.layout = probe.layout;
    pipeline.stage = vkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = probe.shader;
    pipeline.stage.pName = "main";
    REQUIRE(vkCreateComputePipelines(probe.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &probe.pipeline) == VK_SUCCESS);

    REQUIRE(context->oneShot([&](VkCommandBuffer commands) {
        const float clear[4] = {0, 0, 0, 1};
        scene.begin(commands, clear);
        VkClearAttachment depth{};
        depth.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth.clearValue.depthStencil = {0.5f, 0};
        const VkClearRect rect{{{0, 0}, scene.extent()}, 0, 1};
        vkCmdClearAttachments(commands, 1, &depth, 1, &rect);
        scene.end(commands);
        const VkDescriptorSet set = (*heap)->descriptorSet();
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe.layout, 0, 1, &set, 0, nullptr);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe.pipeline);
        const VkDeviceAddress address = output->deviceAddress();
        const std::array<std::uint32_t, 3> push{static_cast<std::uint32_t>(address),
            static_cast<std::uint32_t>(address >> 32), scene.depthSlot()};
        vkCmdPushConstants(commands, probe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push.data());
        vkCmdDispatch(commands, 1, 1, 1);
        VkMemoryBarrier2 barrier = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dep);
    }));
    output->invalidate();
    const auto* values = static_cast<const float*>(output->mapped());
    CHECK(values[0] == 0.0f); // reference < stored depth: occluded.
    CHECK(values[1] == 1.0f); // reference > stored depth: visible.
    CHECK(values[2] == 1.25f);
    CHECK(values[3] == -3.5f);
    CHECK(values[4] == 1.0f); // reference == stored depth: visible.
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
}
