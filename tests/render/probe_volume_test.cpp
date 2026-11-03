#include "rayquery_probe.hpp"
#include "shared_context.hpp"
#include "lighting/light.hpp"
#include "render/bake_setup.hpp"
#include "assets/vmt.hpp"
#include "render/probe_volume.hpp"
#include "render/render_target.hpp"
#include "render/scene_target.hpp"

#include <doctest/doctest.h>
#include <cstring>
#include <fstream>
#include <filesystem>

using namespace apex;
using namespace apex::render;

namespace {
struct Sample { Vec4 point, normal; };

struct Rooms {
    std::shared_ptr<lighting::TriangleMesh> mesh = std::make_shared<lighting::TriangleMesh>();
    std::vector<BrushVertex> vertices;
    Buffer gpuVertices, gpuIndices;
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
        const Vec3 normal = glm::normalize(glm::cross(b - a, c - a));
        const auto first = static_cast<std::uint32_t>(vertices.size());
        for (auto p : {a, b, c, d}) {
            vertices.push_back({{p.x, p.y, p.z}, {normal.x, normal.y, normal.z}, {0, 0}, 0});
            mesh->positions.push_back({p.x, p.y, p.z});
        }
        for (auto i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh->indices.push_back(first + i);
    }
    void box(Vec3 lo, Vec3 hi) {
        quad({lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z});
        quad({hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z});
        quad({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z});
        quad({lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z});
        quad({lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z});
        quad({lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z});
    }
    explicit Rooms(Context& context) {
        box({-208, -208, -144}, {208, 208, -128});
        box({-208, -208, 128}, {208, 208, 144});
        box({-208, -208, -128}, {-192, 208, 128});
        box({192, -208, -128}, {208, 208, 128});
        box({-192, -208, -128}, {192, -192, 128});
        box({-192, 192, -128}, {192, 208, 128});
        box({-8, -192, -128}, {8, 192, 128}); // Actual 16-unit solid separating two sealed rooms.
        auto vb = Buffer::create(context, {vertices.size() * sizeof(BrushVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, MemoryUsage::Upload});
        auto ib = Buffer::create(context, {mesh->indices.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, MemoryUsage::Upload});
        REQUIRE(vb); REQUIRE(ib);
        gpuVertices = std::move(*vb); gpuIndices = std::move(*ib);
        std::memcpy(gpuVertices.mapped(), vertices.data(), gpuVertices.size()); gpuVertices.flush();
        std::memcpy(gpuIndices.mapped(), mesh->indices.data(), gpuIndices.size()); gpuIndices.flush();
    }
    DrawList list() const {
        DrawList list;
        list.mode = DisplayMode::Lit; list.realtimeIndirect = true;
        list.probeSettings.dims = {8, 8, 6};
        list.probeSettings.probesPerFrame = 384;
        list.probeSettings.hysteresis = 0.5f;
        MeshDraw draw;
        draw.vertices = &gpuVertices; draw.indices = &gpuIndices;
        draw.indexCount = static_cast<std::uint32_t>(mesh->indices.size());
        draw.cpuMesh = mesh; draw.texture = 0;
        list.meshes.push_back(draw);
        lighting::Light light;
        light.kind = lighting::LightKind::Spot;
        light.origin = {-96, 0, 80}; light.direction = {0, 0, -1};
        light.intensity = DVec3(30000); light.quadratic = 1;
        light.stopdot = 0.8; light.stopdot2 = 0.6;
        light.indirectMode = 2;
        list.lights.push_back(light);
        return list;
    }
};
std::vector<Sample> ceiling(float x) {
    std::vector<Sample> samples;
    for (int y = -2; y <= 2; ++y) for (int dx = -2; dx <= 2; ++dx)
        samples.push_back({Vec4(x + float(dx) * 12, float(y) * 12, 127.9f, 0), Vec4(0, 0, -1, 0)});
    return samples;
}
std::vector<Vec4> sample(Context& context, const FrameBlock& frame, const std::vector<Sample>& samples) {
    auto input = Buffer::create(context, {samples.size() * sizeof(Sample), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
    auto frames = Buffer::create(context, {sizeof(frame), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
    auto output = Buffer::create(context, {samples.size() * sizeof(Vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Readback});
    REQUIRE(input); REQUIRE(frames); REQUIRE(output);
    std::memcpy(input->mapped(), samples.data(), input->size()); input->flush();
    std::memcpy(frames->mapped(), &frame, sizeof(frame)); frames->flush();
    testing::Probe probe{context.device()};
    auto module = vkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    module.codeSize = sizeof(shaders::probe_sample_comp); module.pCode = shaders::probe_sample_comp;
    REQUIRE(vkCreateShaderModule(probe.device, &module, nullptr, &probe.shader) == VK_SUCCESS);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    auto layout = vkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
    REQUIRE(vkCreatePipelineLayout(probe.device, &layout, nullptr, &probe.layout) == VK_SUCCESS);
    auto pipeline = vkStruct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipeline.layout = probe.layout;
    pipeline.stage = vkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = probe.shader; pipeline.stage.pName = "main";
    REQUIRE(vkCreateComputePipelines(probe.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &probe.pipeline) == VK_SUCCESS);
    REQUIRE(context.oneShot([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe.pipeline);
        const std::array<VkDeviceAddress, 3> addresses{frames->deviceAddress(), input->deviceAddress(), output->deviceAddress()};
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
    auto* values = static_cast<const Vec4*>(output->mapped());
    return {values, values + samples.size()};
}
double luminance(const std::vector<Vec4>& values) {
    double result = 0;
    for (const auto& value : values) result += glm::dot(Vec3(value), Vec3(0.2126f, 0.7152f, 0.0722f)) * 0.65;
    return result / static_cast<double>(values.size());
}
void accumulate(Context& context, ProbeVolume& probes, const DrawList& list, const GpuMaterial& material, int frames = 16) {
    for (int i = 0; i < frames; ++i) {
        REQUIRE(probes.update(list, std::span(&material, 1)));
        REQUIRE(context.oneShot([&](VkCommandBuffer commands) { probes.record(commands); }));
    }
}
void clean(Context& context) {
    for (const auto& error : context.validation().errors()) MESSAGE(error);
    CHECK(context.validation().errorCount() == 0);
    CHECK(context.validation().warningCount() == 0);
}
std::vector<GpuIrradianceProbe> readProbes(Context& context, ProbeVolume& probes, glm::uvec3 dims, bool coarse = false) {
    const std::size_t size = std::size_t(dims.x) * dims.y * dims.z * sizeof(GpuIrradianceProbe);
    auto staging = Buffer::create(context, {size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback});
    REQUIRE(staging);
    REQUIRE(context.oneShot([&](VkCommandBuffer commands) {
        const VkBufferCopy copy{0, 0, size};
        vkCmdCopyBuffer(commands, coarse ? probes.coarseBuffer() : probes.buffer(), staging->handle(), 1, &copy);
        auto memory = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        memory.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; memory.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; memory.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        auto dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(commands, &dep);
    }));
    staging->invalidate();
    const auto* data = static_cast<const GpuIrradianceProbe*>(staging->mapped());
    return {data, data + size / sizeof(GpuIrradianceProbe)};
}
}

TEST_CASE("render: realtime probes bounce deterministically and honour indirect light modes") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    if (!context->info().rayQuery) { MESSAGE("Skipped: ray query unavailable"); return; }
    context->validation().clear();
    {
        Rooms rooms(*context);
        auto list = rooms.list();
        GpuMaterial material; material.bounce = Vec4(0.65f, 0.65f, 0.65f, 0);
        auto created = ProbeVolume::create(*context); REQUIRE(created);
        auto& probes = **created;
        const auto surface = ceiling(-96);
        for (const auto& p : surface)
            CHECK(lighting::irradiance(list.lights[0], DVec3(p.point), DVec3(p.normal)) == DVec3(0));
        const auto off = sample(*context, FrameBlock{}, surface);
        CHECK(luminance(off) == 0);
        accumulate(*context, probes, list, material);
        FrameBlock frame; probes.pack(frame, DVec3(0));
        const auto on = sample(*context, frame, surface);
        MESSAGE("ceiling linear mean off=" << luminance(off) << " probes=" << luminance(on));
        // Cosine-weighted indirect transport in this deterministic room reads
        // ~0.072; using the direct-light surface response instead read ~0.0585.
        CHECK(luminance(on) > 0.064);
        probes.reset();
        accumulate(*context, probes, list, material);
        probes.pack(frame, DVec3(0));
        const auto repeat = sample(*context, frame, surface);
        CHECK(std::memcmp(on.data(), repeat.data(), on.size() * sizeof(Vec4)) == 0);
        // None injects nothing. Static is what the bake will include, so it matches Dynamic exactly.
        list.lights[0].indirectMode = 0;
        accumulate(*context, probes, list, material);
        probes.pack(frame, DVec3(0));
        CHECK(luminance(sample(*context, frame, surface)) == 0);
        list.lights[0].indirectMode = 1;
        probes.reset();
        accumulate(*context, probes, list, material);
        probes.pack(frame, DVec3(0));
        const auto staticMode = sample(*context, frame, surface);
        CHECK(std::memcmp(on.data(), staticMode.data(), on.size() * sizeof(Vec4)) == 0);
        list.lights[0].indirectMode = 2;
        probes.reset();
        material.bounce = Vec4(0.1f, 0.1f, 0.1f, 0);
        accumulate(*context, probes, list, material);
        probes.pack(frame, DVec3(0));
        CHECK(luminance(sample(*context, frame, surface)) < luminance(on) * 0.4);
    }
    clean(*context);
}

TEST_CASE("render: realtime probe visibility rejects a 16 unit wall at 64 unit spacing") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    if (!context->info().rayQuery) { MESSAGE("Skipped: ray query unavailable"); return; }
    context->validation().clear();
    {
        Rooms rooms(*context);
        auto list = rooms.list();
        GpuMaterial material; material.bounce = Vec4(0.65f, 0.65f, 0.65f, 0);
        auto created = ProbeVolume::create(*context); REQUIRE(created);
        for (const float shift : {0.0f, 16.0f, 32.0f}) {
            list.meshes[0].origin.x = shift; // Transform-only TLAS refit, including probes inside the wall.
            list.lights[0].origin.x = -96 + shift;
            accumulate(*context, **created, list, material);
            FrameBlock frame; (*created)->pack(frame, DVec3(0));
            const auto bright = luminance(sample(*context, frame, ceiling(-96 + shift)));
            const auto dark = std::max(luminance(sample(*context, frame, ceiling(96 + shift))),
                                       luminance(sample(*context, frame, ceiling(34 + shift))));
            MESSAGE("wall shift=" << shift << " bright=" << bright << " dark=" << dark << " ratio=" << dark / bright);
            CHECK(bright > 0.01);
            CHECK(dark < bright * 0.05);
        }
        // Empty topology, resize, re-enable and grid shift exercise descriptor/buffer replacement.
        list.meshes.clear();
        REQUIRE((*created)->update(list, std::span(&material, 1)));
        FrameBlock empty; (*created)->pack(empty, DVec3(0));
        CHECK(empty.indirectDims == glm::uvec4(0));
        list = rooms.list(); list.probeSettings.dims = {6, 6, 6}; list.view.cameraPosition.x = 64;
        accumulate(*context, **created, list, material, 2);
        FrameBlock moved; (*created)->pack(moved, list.view.cameraPosition);
        CHECK(moved.indirectDims.x == 6);
    }
    clean(*context);
}

TEST_CASE("render: probe cascades cover the fine edge without a seam") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    if (!context->info().rayQuery) { MESSAGE("Skipped: ray query unavailable"); return; }
    context->validation().clear();
    {
        Rooms rooms(*context);
        auto list = rooms.list();
        GpuMaterial material; material.bounce = Vec4(0.65f, 0.65f, 0.65f, 0);
        auto created = ProbeVolume::create(*context); REQUIRE(created);
        auto& probes = **created;

        // Fine grid {6,6,6} at 64 units covers |x|,|y| <= 160; the coarse cascade ({3,3,3} at 256)
        // covers |x|,|y| <= 256. The lit left-room ceiling at x = -190 is outside the fine grid.
        list.probeSettings.dims = {6, 6, 6};
        std::vector<Sample> sweep;
        for (float x = -240.0f; x <= -40.0f; x += 5.0f) sweep.push_back({Vec4(x, 0, 127.9f, 0), Vec4(0, 0, -1, 0)});
        const auto at = [&](float x) { return std::size_t((x + 240.0f) / 5.0f + 0.5f); };
        accumulate(*context, probes, list, material);
        FrameBlock frame; probes.pack(frame, DVec3(0));
        REQUIRE(frame.indirectCoarseDims == glm::uvec4(3, 3, 3, 0));
        REQUIRE(frame.indirectCoarseParams.x == doctest::Approx(256.0));
        const auto cascaded = sample(*context, frame, sweep);
        std::vector<double> values;
        for (const auto& value : cascaded) values.push_back(luminance({value}));
        double worst = 0, brightest = 0;
        for (std::size_t i = 0; i < values.size(); ++i) {
            brightest = std::max(brightest, values[i]);
            if (i) worst = std::max(worst, std::abs(values[i] - values[i - 1]));
        }
        MESSAGE("cascaded: inside(-60)=" << values[at(-60)] << " fine edge(-160)=" << values[at(-160)]
                << " outside(-190)=" << values[at(-190)] << " brightest=" << brightest
                << " worst adjacent delta (5 units)=" << worst);
        CHECK(values[at(-60)] > 0.005);
        CHECK(values[at(-190)] > 1e-4); // Coarse coverage where the fine grid has none.
        // Stated tolerance: adjacent 5-unit steps change by at most 15% of the brightest sample.
        // A hard fine-to-coarse switch jumps by the fine/coarse difference, far above that.
        CHECK(worst <= 0.15 * brightest);

        // Without the coarse cascade the same outside point is exactly black.
        probes.reset();
        list.probeSettings.cascades = false;
        accumulate(*context, probes, list, material);
        probes.pack(frame, DVec3(0));
        REQUIRE(frame.indirectCoarseDims == glm::uvec4(0));
        const auto single = sample(*context, frame, sweep);
        CHECK(luminance({single[at(-190)]}) == 0.0);
        CHECK(luminance({single[at(-60)]}) > 0.005);
    }
    clean(*context);
}

TEST_CASE("render: probe relocation moves a buried probe into air") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    if (!context->info().rayQuery) { MESSAGE("Skipped: ray query unavailable"); return; }
    context->validation().clear();
    {
        Rooms rooms(*context);
        auto list = rooms.list();
        list.meshes[0].origin.x = 32; // The wall spans [24, 40]; probe cell x=32 lies inside it.
        list.lights[0].origin.x = -96 + 32;
        GpuMaterial material; material.bounce = Vec4(0.65f, 0.65f, 0.65f, 0);
        auto created = ProbeVolume::create(*context); REQUIRE(created);
        auto& probes = **created;
        const glm::uvec3 grid = list.probeSettings.dims;
        // Camera at 0 → fine origin x = -224, cells every 64: x=32 (cell 4), y=-32 (cell 3),
        // z=-32 (cell 2) sit inside the wall.
        const glm::uvec3 buried{4, 3, 2};
        const auto verify = [&](bool relocate) {
            probes.reset();
            list.probeSettings.relocate = relocate;
            accumulate(*context, probes, list, material);
            const auto maps = readProbes(*context, probes, grid);
            return maps[(buried.z * grid.y + buried.y) * grid.x + buried.x];
        };
        const auto buriedProbe = verify(false);
        CHECK(buriedProbe.irradiance[0].w == -1.0f);
        const auto relocated = verify(true);
        CHECK(relocated.irradiance[0].w == 1.0f);
        // The stored offset moves the probe's sampling position out of the wall, bounded by
        // the relocation budget (half a probe spacing).
        const Vec3 offset(relocated.depth[0].z, relocated.depth[0].w, relocated.depth[1].z);
        const double x = 32.0 + double(offset.x) * 64.0;
        MESSAGE("relocation offset=" << offset.x << ' ' << offset.y << ' ' << offset.z << " world x=" << x);
        CHECK(std::abs(offset.x) <= 0.5 + 0.01);
        CHECK(std::abs(offset.y) <= 0.5 + 0.01);
        CHECK(std::abs(offset.z) <= 0.5 + 0.01);
        const bool movedOut = x <= 24.0 - 0.05 || x >= 40.0 + 0.05;
        CHECK(movedOut);
    }
    clean(*context);
}

TEST_CASE("render: probe reflectivity shares the baker override rule") {
    assets::Material material;
    CHECK(bakeReflectivity(material, Vec3(0.1f, 0.3f, 0.6f)) == Vec3(0.1f, 0.3f, 0.6f));
    material.params["$reflectivity"] = "[0.9 0.8 0.7]";
    CHECK(bakeReflectivity(material, Vec3(0.1f)) == Vec3(0.9f, 0.8f, 0.7f));
    material.params["$reflectivity"] = "[-1 2 0.4]";
    CHECK(bakeReflectivity(material, Vec3(0.1f)) == Vec3(0, 1, 0.4f));
}

TEST_CASE("render: probe budgets preserve untouched history and sky replaces CPU probes") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    if (!context->info().rayQuery) { MESSAGE("Skipped: ray query unavailable"); return; }
    context->validation().clear();
    {
        Rooms rooms(*context);
        auto list = rooms.list(); list.probeSettings.probesPerFrame = 32;
        list.probeSettings.cascades = false; // Fine-cascade history budgeting; cascades have their own test.
        GpuMaterial material; material.bounce = Vec4(0.65f, 0.65f, 0.65f, 0);
        auto created = ProbeVolume::create(*context); REQUIRE(created);
        auto& probes = **created;
        accumulate(*context, probes, list, material, 1);
        FrameBlock frame; probes.pack(frame, DVec3(0));
        CHECK(luminance(sample(*context, frame, ceiling(-96))) == 0);
        accumulate(*context, probes, list, material, 11);
        probes.pack(frame, DVec3(0));
        const auto full = sample(*context, frame, ceiling(-96));
        CHECK(luminance(full) > 0.005);
        // Next frame updates only the first z slice; ceiling neighbours must be copied intact.
        accumulate(*context, probes, list, material, 1);
        probes.pack(frame, DVec3(0));
        const auto partial = sample(*context, frame, ceiling(-96));
        CHECK(std::memcmp(full.data(), partial.data(), full.size() * sizeof(Vec4)) == 0);
        list.probeSettings.probesPerFrame = 384;
        list.meshes[0].skySurface = true;
        list.lights[0].kind = lighting::LightKind::Ambient;
        list.lights[0].intensity = DVec3(1);
        accumulate(*context, probes, list, material, 1);
        probes.pack(frame, DVec3(0));
        const std::vector<Sample> points{{Vec4(-96, 0, 0, 0), Vec4(0, 0, 1, 0)},
                                       {Vec4(-96, 0, 0, 0), Vec4(1, 0, 0, 0)}};
        const auto sky = sample(*context, frame, points);
        CHECK(sky[0].x == doctest::Approx(1.0).epsilon(0.1));
        CHECK(sky[1].x == doctest::Approx(0.5).epsilon(0.1));
        FrameBlock rebased; probes.pack(rebased, DVec3(100, 200, 300));
        auto relative = points;
        for (auto& p : relative) p.point -= Vec4(100, 200, 300, 0);
        const auto same = sample(*context, rebased, relative);
        CHECK(std::memcmp(sky.data(), same.data(), sky.size() * sizeof(Vec4)) == 0);
    }
    clean(*context);
}

TEST_CASE("render: realtime indirect is isolated from disabled baked and fullbright output") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    context->validation().clear();
    {
        Rooms rooms(*context);
        auto heap = TextureHeap::create(*context); REQUIRE(heap);
        auto renderer = Renderer::create(*context, **heap, RenderTarget::kColorFormat, RenderTarget::kDepthFormat); REQUIRE(renderer);
        auto target = RenderTarget::create(*context, 32, 32); REQUIRE(target);
        SceneTarget scene(*context, **heap); REQUIRE(scene.resize(32, 32));
        auto list = rooms.list();
        list.cullBackFaces = false;
        // Orthographic clip transform seeing the ceiling from within the left room.
        list.view.cameraPosition = {-96, 0, 0};
        list.view.viewProjection = Mat4(0);
        list.view.viewProjection[0][0] = 1.0f / 64;
        list.view.viewProjection[1][1] = 1.0f / 64;
        list.view.viewProjection[2][2] = -1.0f / 512;
        list.view.viewProjection[3][2] = 1;
        list.view.viewProjection[3][3] = 1;
        const float clear[4] = {0, 0, 0, 1};
        const auto image = [&] {
            REQUIRE(context->oneShot([&](VkCommandBuffer commands) {
                (*renderer)->prepare(commands, list, scene, clear);
                target->begin(commands, clear);
                (*renderer)->record(commands, list);
                target->end(commands);
            }));
            auto pixels = target->readback(); REQUIRE(pixels);
            return pixels->pixels;
        };
        list.realtimeIndirect = false;
        const auto off = image();
        list.realtimeIndirect = true;
        image();
        const auto on = image();
        const auto mean = [](const auto& pixels) {
            double sum = 0;
            for (std::size_t i = 0; i < pixels.size(); i += 4) sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            return sum / (static_cast<double>(pixels.size() / 4) * 3 * 255);
        };
        if (context->info().rayQuery) CHECK(mean(on) > mean(off) + 0.01);
        else CHECK(on == off);
        MESSAGE("rendered ceiling display mean off=" << mean(off) << " probes=" << mean(on));
        if (const char* capture = std::getenv("APEX_GI_CAPTURE_DIR")) {
            std::filesystem::create_directories(capture);
            const auto save = [&](const char* name, const auto& pixels) {
                std::ofstream file(std::filesystem::path(capture) / name, std::ios::binary);
                file << "P6\n32 32\n255\n";
                for (std::size_t i = 0; i < pixels.size(); i += 4)
                    file.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
            };
            save("ceiling-off.ppm", off); save("ceiling-probes.ppm", on);
        }
        list.realtimeIndirect = false;
        CHECK(image() == off);
        for (const bool baked : {false, true}) {
            list.mode = baked ? DisplayMode::Baked : DisplayMode::Lit;
            list.fullbright = !baked;
            list.realtimeIndirect = false;
            const auto reference = image();
            list.realtimeIndirect = true;
            CHECK(image() == reference);
        }
    }
    clean(*context);
}
