#include "render/scene_target.hpp"
#include "render/gpu_lighting.hpp"
#include "render/test_shaders_spv.hpp"
#include "shared_context.hpp"
#include "sky_fixture.hpp"
#include "lighting/normal_map.hpp"
#include "lighting/specular.hpp"

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

TEST_CASE("render: light normal and specular material GLSL transcription matches CPU model") {
    Context* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan 1.3 device"); return; }
    context->validation().clear();
    struct alignas(16) Sample {
        GpuLight light; Vec4 point; Vec4 normal;
        VkDeviceAddress frame; std::uint32_t pad[2]{};
        Vec4 texel{}, dpdx{}, dpdy{}, uv{};
        GpuMaterial material{};
    };
    static_assert(sizeof(Sample) == 336);
    static_assert(offsetof(Sample, material) == 192);
    const auto grid = testing::skyRoomGrid();
    REQUIRE(grid.validCount() > 0);
    REQUIRE(grid.validCount() < grid.probeCount());
    std::vector<GpuSkyProbe> packed;
    const auto dims = grid.dims();
    for (int z = 0; z < dims.z; ++z)
        for (int y = 0; y < dims.y; ++y)
            for (int x = 0; x < dims.x; ++x) packed.push_back(packSkyProbe(grid.probe(x, y, z)));
    auto probes = Buffer::create(*context, {packed.size() * sizeof(GpuSkyProbe),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
    auto frames = Buffer::create(*context, {2 * sizeof(FrameBlock),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
    REQUIRE(probes); REQUIRE(frames);
    std::memcpy(probes->mapped(), packed.data(), packed.size() * sizeof(GpuSkyProbe));
    probes->flush();
    const DVec3 camera{31, -73, 19};
    std::array<FrameBlock, 2> frameData{};
    packSkyGrid(frameData[1], grid, probes->deviceAddress(), camera);
    std::memcpy(frames->mapped(), frameData.data(), sizeof(frameData));
    frames->flush();
    std::vector<Sample> samples;
    std::vector<DVec3> expected;
    std::mt19937 random(0x644);
    std::uniform_real_distribution<double> unit(-1, 1), positive(0.1, 1);
    const auto direction = [&]() { return glm::normalize(DVec3(unit(random), unit(random), unit(random))); };
    const auto add = [&](lighting::Light light, DVec3 point, DVec3 normal, bool sky = false) {
        // Probe positions/normals are explicitly float inputs; the model still evaluates in double.
        point = DVec3(Vec3(point));
        normal = DVec3(Vec3(normal));
        samples.push_back({packLight(light, camera), Vec4(Vec3(point - camera), 0), Vec4(Vec3(normal), 0),
                           frames->deviceAddress() + (sky ? sizeof(FrameBlock) : 0), {}});
        expected.push_back(light.directMode == 0 ? DVec3(0) : light.kind == lighting::LightKind::Ambient ?
                           light.intensity * (sky ? grid.sample(point, normal) : 0.0) :
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
    add(edge, {0, 0, 0}, {0, 0, 1}); // No grid: no accidental fill.
    for (int i = 0; i < 1000; ++i)
        add(edge, {512 + unit(random) * 1000, unit(random) * 400, 256 + unit(random) * 400}, direction(), true);
    for (const DVec3 point : {DVec3(0, 0, 0), DVec3(1024, 0, 0), DVec3(512, 0, 256)})
        for (const DVec3 normal : {DVec3(0, 0, 1), DVec3(1, 0, 0), DVec3(0, 0, -1)})
            add(edge, point, normal, true);
    edge.directMode = 0;
    add(edge, {0, 0, 0}, {0, 0, 1}, true);

    for (int i = 0; i < 400; ++i) {
        Sample sample{};
        sample.pad[0] = i % 3 == 0 ? 1u : 0u;
        sample.texel = Vec4(Vec3(positive(random), positive(random), positive(random)), float(1 + i % 4));
        sample.normal = Vec4(Vec3(direction()), 0);
        sample.dpdx = Vec4(Vec3(direction() * 4.0), 0);
        sample.dpdy = Vec4(Vec3(direction() * 3.0), 0);
        sample.uv = Vec4(float(unit(random)), float(unit(random)), float(unit(random)), float(unit(random)));
        if (i % 11 == 0) sample.uv = Vec4(0); // Collapsed UVs.
        if (i % 13 == 0) sample.texel = Vec4(i % 2 == 0 ? Vec3(0) : Vec3(0.5f), sample.texel.w);
        auto normal = lighting::decodeNormal(DVec3(sample.texel), sample.texel.w == 2 || sample.texel.w == 4, sample.pad[0] != 0);
        if (sample.texel.w >= 3) normal = lighting::perturbNormal(DVec3(sample.normal), DVec3(sample.dpdx),
            DVec3(sample.dpdy), DVec2(sample.uv), DVec2(sample.uv.z, sample.uv.w), normal);
        samples.push_back(sample);
        expected.push_back(normal);
    }
    for (int i = 0; i < 640; ++i) {
        Sample s{};
        s.pad[1] = 1u + i % 4; // GGX, phong; then each with measured light irradiance.
        s.normal = Vec4(Vec3(direction()), 0);
        s.dpdx = Vec4(Vec3(direction()), 0); // view
        s.dpdy = Vec4(Vec3(direction()), 0); // to light
        s.texel = Vec4(float(positive(random)), float(positive(random)), float(positive(random)), float(positive(random)));
        s.material.mraoScale = Vec4(float(positive(random)), float(positive(random)), 1, 0);
        s.material.phongParams = Vec4(float(positive(random) * 150), float(positive(random) * 4), 0, 0);
        s.material.phongFresnel = Vec4(0.1f, 0.3f, 0.9f, 0);
        s.material.phongTint = Vec4(0.2f, 0.5f, 0.9f, 0);
        s.material.phongExponentSlot = (i / 4) % 2 == 0 ? kNoTexture : 0u;
        s.uv.x = float(positive(random)); // Synthetic exponent-texture R.
        if (i < 16) {
            s.normal = s.dpdx = s.dpdy = Vec4(0, 0, 1, 0);
            s.material.mraoScale = Vec4(float(i % 2), i < 8 ? 0.0f : 1.0f, 1, 0);
            s.uv.x = float(i % 2);
        }
        lighting::Light light;
        light.kind = static_cast<lighting::LightKind>(i % 5 == 4 ? 5 : 1 + i % 3);
        light.intensity = DVec3(0.5, 0.7, 1);
        light.origin = DVec3(Vec3(s.dpdy)) * 40.0;
        light.direction = -DVec3(Vec3(s.dpdy));
        light.constant = 1;
        light.stopdot = 0.9;
        light.stopdot2 = 0.5;
        light.exponent = 2;
        light.directMode = i % 7 == 0 ? 0 : 1;
        s.light = packLight(light, DVec3(0));
        s.frame = frames->deviceAddress(); // Ambient without a grid contributes zero.
        const DVec3 n(s.normal), v(s.dpdx), l(s.dpdy);
        const auto& m = s.material;
        DVec3 brdf = s.pad[1] % 2 == 1 ?
            lighting::ggxBrdf(n, v, l, DVec3(s.texel), m.mraoScale.x, m.mraoScale.y) :
            lighting::phongBrdf(n, v, l, m.phongExponentSlot == kNoTexture ? m.phongParams.x :
                lighting::phongTextureExponent(s.uv.x), m.phongParams.y, DVec3(m.phongFresnel),
                DVec3(m.phongTint), s.texel.w);
        if (s.pad[1] > 2) {
            const auto energy = light.directMode == 0 || light.kind == lighting::LightKind::Ambient ? DVec3(0) :
                lighting::irradiance(light, DVec3(0), n);
            brdf = lighting::specularIrradiance(energy, glm::dot(n, l), brdf);
        }
        samples.push_back(s);
        expected.push_back(brdf);
    }
    // Envmap BRDF fit with light scale (5), non-PBR mask selection (6), self-illumination (7)
    // and $envmapcontrast/$envmapsaturation (8).
    for (int i = 0; i < 800; ++i) {
        Sample s{};
        s.pad[1] = 5u + i % 4;
        s.texel = Vec4(float(positive(random)), float(positive(random)), float(positive(random)), float(positive(random)));
        s.dpdx = Vec4(float(positive(random) * 3), float(positive(random)), float(positive(random)), 0);
        s.uv = Vec4(float(unit(random) * 0.6 + 0.5), float(unit(random) * 0.6 + 0.5), 0, 0); // Includes out-of-range.
        if (i < 30) s.uv = Vec4(float(i % 3) * 0.5f, float((i / 3) % 5) * 0.25f, 0, 0); // Endpoints and knots.
        auto& m = s.material;
        m.flags = (i / 3) % 4 == 1 ? kMaterialNormalAlphaEnvmapMask : (i / 3) % 4 == 2 ? kMaterialBaseAlphaEnvmapMask :
                  (i / 3) % 4 == 3 ? kMaterialNormalAlphaEnvmapMask | kMaterialBaseAlphaEnvmapMask : 0u;
        m.envmapMaskSlot = (i / 12) % 2 == 0 ? kNoTexture : 3u;
        m.selfIllumMaskSlot = (i / 6) % 2 == 0 ? kNoTexture : 4u;
        m.envmapTint = Vec4(float(positive(random)), float(positive(random)), float(positive(random)), float(positive(random)));
        m.selfIllumTint = Vec4(float(positive(random)), float(positive(random)), float(positive(random)), 0);
        m.envmapParams = Vec4(float(positive(random)), float(positive(random) * 2), 0, 0);
        if (i < 40) m.envmapParams = Vec4(float(i % 2), float((i / 2) % 3) * 0.5f, 0, 0); // Endpoints.
        const DVec3 texel(s.texel), maskTexel(s.dpdx), envTint(m.envmapTint);
        DVec3 value = s.pad[1] == 5 ?
            lighting::envBrdf(texel, s.uv.x, s.uv.y) * lighting::envmapLightScale(maskTexel, m.envmapTint.w) :
            s.pad[1] == 6 ? envTint * lighting::envmapMask(m.envmapMaskSlot != kNoTexture, maskTexel,
                (m.flags & kMaterialNormalAlphaEnvmapMask) != 0, s.uv.x, (m.flags & kMaterialBaseAlphaEnvmapMask) != 0, s.texel.w) :
            s.pad[1] == 8 ? lighting::envmapContrastSaturation(texel, m.envmapParams.x, m.envmapParams.y) :
            DVec3(m.selfIllumTint) * lighting::selfIllumMask(m.selfIllumMaskSlot != kNoTexture, s.uv.y, s.texel.w);
        samples.push_back(s);
        expected.push_back(value);
    }
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
            CHECK(std::abs(values[i][c] - expected[i][c]) <= 1e-4 * std::max(std::abs(expected[i][c]), 1e-3));
        }
    }
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
}

TEST_CASE("material normal model preserves flat normals slopes and mirrored texture axes") {
    CHECK(lighting::decodeNormal({0.5, 0.5, 1}, false) == DVec3(0, 0, 1));
    CHECK(lighting::decodeNormal({0, 0, 0}, true) == DVec3(0, 0, 1));
    CHECK(glm::length(lighting::decodeNormal({1, 1, 1}, true) - DVec3(0, 0, 1)) < 1e-12);
    CHECK(lighting::decodeNormal({0.5, 0.5, 0}, false, true) == DVec3(0, 0, 1));
    CHECK(lighting::decodeNormal({0.5, 1, 0.5}, false) == DVec3(0, 1, 0));
    for (double mirror : {-1.0, 1.0}) {
        const DVec3 slope = glm::normalize(DVec3(0.3, 0.7, 1));
        const auto normal = lighting::perturbNormal({0, 0, 1}, {2, 0, 0}, {0, 3, 0},
            {mirror * 0.25, 0}, {0, 2}, slope);
        CHECK(glm::length(normal - DVec3(mirror * slope.x, slope.y, slope.z)) < 1e-12);
    }
}
