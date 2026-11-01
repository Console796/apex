#include "render/clusters.hpp"
#include "render/renderer.hpp"
#include "render/shader_blobs.hpp"
#include "render/view_math.hpp"
#include "shared_context.hpp"

#include <doctest/doctest.h>

#include <cstring>
#include <random>

using namespace apex;
using namespace apex::render;

namespace {
struct ClusterProbe {
    VkDevice device;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ~ClusterProbe() {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
    }
};
}

TEST_CASE("render: cluster compute matches CPU brute force for random point spot and unbounded lights") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    context->validation().clear();
    bool overflow = false, ortho = false;
    SUBCASE("perspective") {}
    SUBCASE("orthographic") { ortho = true; }
    SUBCASE("overflow") { overflow = true; }
    const glm::uvec3 dims{5, 3, kClusterSlices};
    const unsigned cells = dims.x * dims.y * dims.z;
    auto output = Buffer::create(*context, {16 + cells * sizeof(GpuCluster),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback});
    REQUIRE(output);
    std::memset(output->mapped(), 0xFF, static_cast<std::size_t>(output->size()));
    output->flush();
    const auto basis = angleVectors(DVec3{13, 27, 0});
    const Mat4 projection = (ortho ? reversedZOrtho(400, 240, 1, 65536) :
        reversedZPerspective(glm::radians(75.0), 5.0 / 3, 1)) * rotationOnlyView(basis);
    const DVec3 camera{1e10, -2e10, 3e10};
    std::vector<GpuLight> lights;
    for (const auto kind : {lighting::LightKind::Sun, lighting::LightKind::Ambient}) {
        lighting::Light light;
        light.kind = kind;
        light.intensity = DVec3(1);
        lights.push_back(packLight(light, camera));
    }
    std::mt19937 random(0x666);
    std::uniform_real_distribution<double> position(-800, 800), intensity(0.001, 1);
    for (unsigned i = 0; i < (overflow ? 79u : 512u); ++i) {
        lighting::Light light;
        light.kind = i % 2 ? lighting::LightKind::Spot : lighting::LightKind::Point;
        light.origin = camera + DVec3{position(random), position(random), position(random)};
        light.intensity = DVec3(intensity(random));
        light.direction = glm::normalize(DVec3{position(random), position(random), position(random)});
        light.stopdot = 0.9; light.stopdot2 = 0.5;
        light.constant = 1;
        if (i == 0) light.intensity = DVec3(0.001);
        light.quadratic = i % 97 ? 0.01 : 0; // Unbounded constant lights, including below cutoff.
        light.radius = i % 5 ? 0 : 80;
        light.directMode = i % 7 ? 2 : 0;
        light.specularMode = i % 4 ? 0 : 2;
        if (overflow) { light.directMode = 2; light.radius = 0; light.quadratic = 0; light.intensity = DVec3(1); }
        auto packed = packLight(light, camera);
        packed.originRadius.w = float(lighting::previewRange(light, 1));
        lights.push_back(packed);
    }
    auto input = Buffer::create(*context, {sizeof(FrameBlock) + lights.size() * sizeof(GpuLight),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
    REQUIRE(input);
    FrameBlock frame;
    frame.lights = input->deviceAddress() + sizeof(FrameBlock);
    frame.lightCount = static_cast<std::uint32_t>(lights.size());
    frame.clusters = output->deviceAddress() + 16;
    frame.clusterStats = output->deviceAddress();
    frame.clusterDims = glm::uvec4(dims, 2);
    frame.clusterProjection = projection;
    std::memcpy(input->mapped(), &frame, sizeof(frame));
    std::memcpy(static_cast<std::byte*>(input->mapped()) + sizeof(frame), lights.data(), lights.size() * sizeof(GpuLight));
    input->flush();
    ClusterProbe probe{context->device()};
    const auto code = builtinSpirv("cluster_comp");
    REQUIRE_FALSE(code.empty());
    auto shader = vkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    shader.codeSize = code.size_bytes(); shader.pCode = code.data();
    REQUIRE(vkCreateShaderModule(probe.device, &shader, nullptr, &probe.shader) == VK_SUCCESS);
    const VkPushConstantRange range{VK_SHADER_STAGE_ALL, 0, sizeof(PushConstants)};
    auto layout = vkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
    REQUIRE(vkCreatePipelineLayout(probe.device, &layout, nullptr, &probe.layout) == VK_SUCCESS);
    auto pipeline = vkStruct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipeline.layout = probe.layout;
    pipeline.stage = vkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = probe.shader; pipeline.stage.pName = "main";
    REQUIRE(vkCreateComputePipelines(probe.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &probe.pipeline) == VK_SUCCESS);
    REQUIRE(context->oneShot([&](VkCommandBuffer commands) {
        vkCmdFillBuffer(commands, output->handle(), 0, 16, 0);
        auto barrier = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        auto dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dep);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe.pipeline);
        PushConstants push{};
        const VkDeviceAddress address = input->deviceAddress();
        std::memcpy(&push.p1, &address, sizeof(address));
        vkCmdPushConstants(commands, probe.layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(commands, (cells + 63) / 64, 1, 1);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        vkCmdPipelineBarrier2(commands, &dep);
    }));
    output->invalidate();
    const auto* clusters = reinterpret_cast<const GpuCluster*>(static_cast<const std::byte*>(output->mapped()) + 16);
    unsigned dropped = 0;
    const auto inverse = glm::inverse(DMat4(projection));
    for (unsigned index = 0; index < cells; ++index) {
        const auto planes = clusterPlanes(projection, dims, {index % dims.x, (index / dims.x) % dims.y, index / (dims.x * dims.y)});
        std::vector<unsigned> expected;
        for (unsigned i = 2; i < lights.size(); ++i) {
            const auto& light = lights[i];
            if ((light.directMode == 1 || light.directMode == 2 || light.specularMode == 2) &&
                clusterIntersects(planes, Vec3(light.originRadius), light.originRadius.w)) expected.push_back(i);
        }
        dropped += expected.size() > kClusterCapacity ? unsigned(expected.size()) - kClusterCapacity : 0;
        expected.resize(std::min(expected.size(), std::size_t(kClusterCapacity)));
        CAPTURE(index); CAPTURE(ortho); CAPTURE(overflow);
        REQUIRE(clusters[index].count == expected.size());
        for (unsigned entry = 0; entry < expected.size(); ++entry) CHECK(clusters[index].indices[entry] == expected[entry]);
        // Independent point brute force: a light reaching an interior sample must survive plane rejection.
        if (clusters[index].count == kClusterCapacity) continue;
        const unsigned z = index / (dims.x * dims.y);
        const double nearDepth = std::exp2(-double(z));
        const double farDepth = z + 1 == dims.z ? 0 : nearDepth * 0.5;
        const auto homogeneous = inverse * DVec4(
            2.0 * (double(index % dims.x) + 0.37) / dims.x - 1,
            2.0 * (double((index / dims.x) % dims.y) + 0.63) / dims.y - 1,
            (nearDepth + farDepth) * 0.5, 1);
        const DVec3 point = DVec3(homogeneous) / homogeneous.w;
        for (unsigned i = 2; i < lights.size(); ++i) {
            const auto& light = lights[i];
            if (light.directMode == 0 && light.specularMode == 0) continue;
            const auto delta = point - DVec3(light.originRadius);
            const double distance = glm::length(delta);
            if (distance > light.originRadius.w || light.originRadius.w <= 0) continue;
            if (light.kind == 2 && glm::dot(delta / distance, DVec3(light.directionInner)) <= light.attenuationOuter.w) continue;
            CHECK(std::find(expected.begin(), expected.end(), i) != expected.end());
        }
    }
    CHECK(*static_cast<const unsigned*>(output->mapped()) == dropped);
    if (overflow) CHECK(dropped == cells * 15);
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
}
