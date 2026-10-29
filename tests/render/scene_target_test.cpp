#include "render/scene_target.hpp"
#include "render/gpu_lighting.hpp"
#include "render/test_shaders_spv.hpp"
#include "shared_context.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <random>

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
    // A test-only compute layout, separate from the renderer's 128-byte push ABI.
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

TEST_CASE("render: light GLSL transcription matches CPU irradiance") {
    Context* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan 1.3 device"); return; }
    context->validation().clear();
    struct alignas(16) Sample { GpuLight light; Vec4 point; Vec4 normal; };
    static_assert(sizeof(Sample) == 112);
    std::vector<Sample> samples;
    std::vector<DVec3> expected;
    std::mt19937 random(0x644);
    std::uniform_real_distribution<double> unit(-1, 1), positive(0.1, 1);
    const auto direction = [&]() { return glm::normalize(DVec3(unit(random), unit(random), unit(random))); };
    const auto add = [&](lighting::Light light, DVec3 point, DVec3 normal) {
        // Probe positions/normals are explicitly float inputs; the model still evaluates in double.
        point = DVec3(Vec3(point));
        normal = DVec3(Vec3(normal));
        samples.push_back({packLight(light, DVec3(0)), Vec4(Vec3(point), 0), Vec4(Vec3(normal), 0)});
        expected.push_back(light.directMode == 0 || light.kind == lighting::LightKind::Ambient ? DVec3(0) :
                           lighting::irradiance(light, point, normal));
    };
    for (int i = 0; i < 600; ++i) {
        lighting::Light light;
        light.kind = static_cast<lighting::LightKind>(1 + i % 3);
        light.origin = DVec3(Vec3(direction() * 100.0));
        light.direction = DVec3(Vec3(direction()));
        light.intensity = DVec3(Vec3(DVec3(positive(random), positive(random), positive(random)) * 100.0));
        light.constant = positive(random);
        light.linear = positive(random) * 0.01;
        light.quadratic = positive(random) * 0.001;
        light.radius = i % 4 == 0 ? 100 : 0;
        light.stopdot = 0.875;
        light.stopdot2 = 0.5;
        light.exponent = i % 4 == 0 ? 0 : i % 4 == 1 ? 1 : i % 4 == 2 ? 2 : 4;
        light.directMode = i % 17 == 0 ? 0 : 1;
        if (i % 5 == 0) {
            auto block = kv1::Node::block("entity");
            for (const auto& [key, value] : std::array<std::pair<const char*, const char*>, 4>{{
                     {"classname", "light"}, {"_light", "255 160 80 200"},
                     {"_fifty_percent_distance", "64"}, {"_zero_percent_distance", "256"}}})
                block.append(kv1::Node::value(key, value)).value();
            const double fifty = 32.0 + 8.0 * (i % 17);
            const double ratio = i % 4 == 0 ? 1.0 : i % 4 == 1 ? 2.0 : i % 4 == 2 ? 4.0 : 8.0;
            block.find("_fifty_percent_distance")->setValue(std::to_string(fifty)).value();
            block.find("_zero_percent_distance")->setValue(std::to_string(fifty * ratio)).value();
            const auto fitted = lighting::lightsFromEntity(block).front();
            light.constant = fitted.constant; light.linear = fitted.linear; light.quadratic = fitted.quadratic;
        }
        add(light, direction() * 150.0, direction());
    }
    // Exact and nearby cone edges, radius boundary, source position and below-horizon normals.
    lighting::Light edge;
    edge.kind = lighting::LightKind::Spot;
    edge.direction = {1, 0, 0}; edge.intensity = {2, 3, 4}; edge.constant = 1;
    edge.stopdot = 0.875; edge.stopdot2 = 0.5;
    for (double exponent : {0.0, 1.0, 2.0, 4.0}) {
        edge.exponent = exponent;
        for (double cosine : {0.49, 0.5, 0.501, 0.7, 0.874, 0.875, 0.876, 1.0}) {
            const DVec3 p = DVec3(Vec3(DVec3(cosine, std::sqrt(1 - cosine * cosine), 0) * 64.0));
            add(edge, p, -glm::normalize(p));
            add(edge, p, glm::normalize(p));
        }
    }
    edge.kind = lighting::LightKind::Point; edge.radius = 64;
    for (double distance : {0.0, 63.999, 64.0, 64.001}) add(edge, {distance, 0, 0}, {-1, 0, 0});
    edge.kind = lighting::LightKind::Sun;
    edge.direction = {0, 0, -1};
    for (const DVec3 normal : {DVec3(0, 0, 1), DVec3(1, 0, 0), DVec3(0, 0, -1)})
        add(edge, {0, 0, 0}, normal);
    edge.kind = lighting::LightKind::Ambient;
    add(edge, {0, 0, 0}, {0, 0, 1}); // Ambient waits for D16; no accidental fill.

    auto input = Buffer::create(*context, {samples.size() * sizeof(Sample),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
    auto output = Buffer::create(*context, {samples.size() * sizeof(Vec4),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Readback});
    REQUIRE(input);
    REQUIRE(output);
    std::memcpy(input->mapped(), samples.data(), samples.size() * sizeof(Sample));
    input->flush();
    Probe probe{context->device()};
    auto shader = vkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    shader.codeSize = sizeof(shaders::lighting_probe_comp); shader.pCode = shaders::lighting_probe_comp;
    REQUIRE(vkCreateShaderModule(probe.device, &shader, nullptr, &probe.shader) == VK_SUCCESS);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    auto layout = vkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
    REQUIRE(vkCreatePipelineLayout(probe.device, &layout, nullptr, &probe.layout) == VK_SUCCESS);
    auto pipeline = vkStruct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipeline.layout = probe.layout;
    pipeline.stage = vkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = probe.shader; pipeline.stage.pName = "main";
    REQUIRE(vkCreateComputePipelines(probe.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &probe.pipeline) == VK_SUCCESS);
    REQUIRE(context->oneShot([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe.pipeline);
        const std::array<VkDeviceAddress, 2> addresses{input->deviceAddress(), output->deviceAddress()};
        vkCmdPushConstants(commands, probe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(addresses), addresses.data());
        vkCmdDispatch(commands, static_cast<std::uint32_t>(samples.size()), 1, 1);
        auto barrier = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        auto dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dep);
    }));
    output->invalidate();
    const auto* values = static_cast<const Vec4*>(output->mapped());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        for (int c = 0; c < 3; ++c) {
            CAPTURE(c);
            // The 1e-3 floor allows 1e-7 absolute error at float cone/horizon boundaries.
            CHECK(std::abs(values[i][c] - expected[i][c]) <= 1e-4 * std::max(expected[i][c], 1e-3));
        }
    }
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
}
