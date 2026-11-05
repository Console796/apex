#include "lighting/baker.hpp"
#include "lighting/bake_session.hpp"
#include "render/gpu_bake.hpp"
#include "shared_context.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace apex;
using namespace apex::lighting;
using namespace apex::render;

namespace {

BakeFace quadFace(DVec3 origin, DVec3 u, DVec3 v, DVec3 normal) {
    const std::vector<DVec3> vertices{origin, origin + 128.0 * u,
        origin + 128.0 * (u + v), origin + 128.0 * v};
    geometry::Face face;
    face.vertices = {0, 1, 2, 3};
    face.plane = {normal, glm::dot(normal, origin)};
    face.uv.uaxis = u;
    face.uv.vaxis = v;
    face.uv.shiftU = 7;
    face.uv.shiftV = -11;
    auto result = bakeFaces(face, vertices);
    REQUIRE(result.size() == 1);
    return std::move(result[0]);
}

struct Casters {
    std::shared_ptr<BakeCasterGeometry> geometry = std::make_shared<BakeCasterGeometry>();

    void quad(DVec3 a, DVec3 b, DVec3 c, DVec3 d, bool sky = false,
              std::optional<std::size_t> face = {}) {
        auto& mesh = geometry->mesh;
        const auto base = static_cast<std::uint32_t>(mesh.positions.size());
        for (const auto point : {a, b, c, d})
            mesh.positions.push_back({static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        geometry->primitives.insert(geometry->primitives.end(), 2, BakeCasterPrimitive{sky, face});
    }

    void faces(const BakeScene& scene) {
        for (std::size_t index = 0; index < scene.faces.size(); ++index) {
            const auto& vertices = scene.faces[index].vertices;
            REQUIRE(vertices.size() == 4);
            quad(vertices[0], vertices[1], vertices[2], vertices[3], false, index);
        }
    }

    BakeOcclusion build() const {
        auto blas = CpuBlas::build(geometry->mesh.positions, geometry->mesh.indices);
        REQUIRE(blas);
        const std::array<CpuInstance, 1> instances{{{*blas,
            {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, 0}}};
        auto accel = CpuAccel::build(instances);
        REQUIRE(accel);
        return {std::shared_ptr<const CpuAccel>(std::move(*accel)),
            [geometry = geometry](const RayHit& hit) { return geometry->primitives.at(hit.primitive).sky; },
            [geometry = geometry](const RayHit& hit) { return geometry->primitives.at(hit.primitive).face; },
            geometry};
    }
};

Context* gpuContext() {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return nullptr; }
    MESSAGE("GPU bake device: " << context->info().name << "; validation=" << context->info().validation);
    if (!context->info().rayQuery) { MESSAGE("Skipped: ray query is not enabled on this device"); return nullptr; }
    return context;
}

std::shared_ptr<BakeKernel> gpuKernel(Context& context, const BakeScene& scene, BakerOptions options) {
    auto created = GpuBakeKernel::create(context, scene, options);
    REQUIRE(created);
    return *created;
}

DVec3 mean(const std::vector<std::vector<Vec3>>& level) {
    DVec3 sum{0};
    std::size_t count = 0;
    for (const auto& face : level) for (const auto value : face) { sum += DVec3(value); ++count; }
    return count ? sum / static_cast<double>(count) : DVec3(0);
}

DVec3 coveredMean(const BakeScene& scene, const std::vector<std::vector<Vec3>>& level) {
    constexpr int sub = 8;
    DVec3 sum{0};
    double totalWeight = 0;
    for (std::size_t faceIndex = 0; faceIndex < level.size(); ++faceIndex) {
        const auto& face = scene.faces[faceIndex];
        DVec3 lo{std::numeric_limits<double>::infinity()}, hi{-std::numeric_limits<double>::infinity()};
        for (const auto& vertex : face.vertices) { lo = glm::min(lo, vertex - 1e-6); hi = glm::max(hi, vertex + 1e-6); }
        const auto width = static_cast<std::size_t>(face.chart.dimensions[0]);
        for (std::size_t index = 0; index < level[faceIndex].size(); ++index) {
            int inside = 0;
            const DVec2 centre{static_cast<double>(index % width), static_cast<double>(index / width)};
            for (int y = 0; y < sub; ++y) for (int x = 0; x < sub; ++x) {
                const DVec2 offset{(x + 0.5) / sub - 0.5, (y + 0.5) / sub - 0.5};
                const auto point = face.chart.worldAt(centre + offset);
                if (glm::all(glm::greaterThanEqual(point, lo)) && glm::all(glm::lessThanEqual(point, hi))) ++inside;
            }
            const double weight = static_cast<double>(inside) / (sub * sub);
            sum += DVec3(level[faceIndex][index]) * weight;
            totalWeight += weight;
        }
    }
    return sum / totalWeight;
}

void checkClose(const std::vector<std::vector<Vec3>>& cpu, const std::vector<std::vector<Vec3>>& gpu,
                double relative, double absolute = 1e-5) {
    REQUIRE(cpu.size() == gpu.size());
    for (std::size_t face = 0; face < cpu.size(); ++face) {
        REQUIRE(cpu[face].size() == gpu[face].size());
        for (std::size_t luxel = 0; luxel < cpu[face].size(); ++luxel) for (int channel = 0; channel < 3; ++channel) {
            INFO("face ", face, " luxel ", luxel, " channel ", channel, " cpu ", cpu[face][luxel][channel],
                 " gpu ", gpu[face][luxel][channel]);
            const double scale = std::max(std::abs(static_cast<double>(cpu[face][luxel][channel])), absolute);
            CHECK(std::abs(static_cast<double>(gpu[face][luxel][channel] - cpu[face][luxel][channel])) <=
                  absolute + relative * scale);
        }
    }
}

BakeScene cornerScene() {
    BakeScene scene;
    scene.faces = {quadFace({-64, -64, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}),
                   quadFace({64, -64, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0})};
    scene.faces[0].albedo = Vec3{0.5f};
    Light light;
    light.kind = LightKind::Spot;
    light.origin = {-32, 0, 96};
    light.direction = {0, 0, -1};
    light.intensity = DVec3{30000};
    light.quadratic = 1;
    light.stopdot = 0.98;
    light.stopdot2 = 0.95;
    scene.lights = {light};
    Casters casters;
    casters.faces(scene);
    scene.occlusion = casters.build();
    return scene;
}

BakeScene sealedRoom() {
    BakeScene scene;
    scene.faces = {
        quadFace({-64, -64, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}),
        quadFace({-64, -64, 128}, {1, 0, 0}, {0, 1, 0}, {0, 0, -1}),
        quadFace({-64, -64, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}),
        quadFace({64, -64, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}),
        quadFace({-64, -64, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}),
        quadFace({-64, 64, 0}, {1, 0, 0}, {0, 0, 1}, {0, -1, 0})};
    for (auto& face : scene.faces) face.albedo = Vec3{0.5f};
    Light light;
    light.origin = {0, 0, 64};
    light.intensity = {30000, 20000, 10000};
    light.quadratic = 1;
    scene.lights = {light};
    Casters casters;
    casters.faces(scene);
    scene.occlusion = casters.build();
    return scene;
}

} // namespace

TEST_CASE("render: gpu bake direct point spot and sun match CPU with occlusion") {
    auto* context = gpuContext();
    if (!context) return;
    context->validation().clear();
    BakeScene scene;
    scene.faces = {quadFace({-64, -64, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1})};
    Casters casters;
    casters.quad({7, -1000, 1.1}, {7, 1000, 1.1}, {7, 1000, 160}, {7, -1000, 160});
    scene.occlusion = casters.build();
    BakerOptions options{.seed = 17, .threads = 1};
    for (const auto kind : {LightKind::Point, LightKind::Spot, LightKind::Sun}) {
        Light light;
        light.kind = kind;
        light.origin = {40, 0, 96};
        light.intensity = {30000, 20000, 10000};
        light.quadratic = 1;
        light.direction = glm::normalize(DVec3{-0.3, 0, -1});
        light.stopdot = 0.95;
        light.stopdot2 = 0.8;
        if (kind == LightKind::Sun) light.intensity = {2, 3, 4};
        scene.lights = {light};
        Baker cpu(scene, options);
        Baker gpu(scene, options, gpuKernel(*context, scene, options));
        cpu.step(); gpu.step();
        const auto cpuResult = cpu.resolve(), gpuResult = gpu.resolve();
        MESSAGE("gpu bake direct kind=" << static_cast<int>(kind) << " cpu=" << mean(cpuResult).x
                << " gpu=" << mean(gpuResult).x);
        checkClose(cpuResult, gpuResult, 1e-3, 2e-5);
    }
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
}

TEST_CASE("render: gpu bake sky and two bounce means match CPU statistically") {
    auto* context = gpuContext();
    if (!context) return;
    context->validation().clear();
    auto scene = cornerScene();
    Light ambient;
    ambient.kind = LightKind::Ambient;
    ambient.intensity = {0.5, 0.75, 1};
    scene.lights.push_back(ambient);
    BakerOptions options{.seed = 73, .threads = 1, .skyRays = 256, .bounces = 2, .bounceRays = 256};
    Baker cpu(scene, options);
    Baker gpu(scene, options, gpuKernel(*context, scene, options));
    for (int pass = 0; pass < 4; ++pass) { cpu.step(); gpu.step(); }
    for (unsigned level = 1; level <= 2; ++level)
        for (int pass = 0; pass < 4; ++pass) { cpu.bounce(level); gpu.bounce(level); }
    for (unsigned level = 0; level <= 2; ++level) {
        const auto cpuResult = cpu.resolveLevel(level), gpuResult = gpu.resolveLevel(level);
        const auto cpuMean = coveredMean(scene, cpuResult), gpuMean = coveredMean(scene, gpuResult);
        MESSAGE("gpu bake level=" << level << " cpu=" << cpuMean.x << " gpu=" << gpuMean.x
                << " ratio=" << (cpuMean.x > 0 ? gpuMean.x / cpuMean.x : 0));
        for (int channel = 0; channel < 3; ++channel)
            CHECK(gpuMean[channel] == doctest::Approx(cpuMean[channel]).epsilon(0.06));
        checkClose(cpuResult, gpuResult, 0.45, std::max(1e-4, cpuMean.x * 0.03));
    }
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
}

TEST_CASE("render: gpu bake sealed room bounce energy decreases") {
    auto* context = gpuContext();
    if (!context) return;
    context->validation().clear();
    const auto scene = sealedRoom();
    BakerOptions options{.seed = 911, .threads = 1, .bounces = 2, .bounceRays = 512};
    Baker gpu(scene, options, gpuKernel(*context, scene, options));
    gpu.step(); gpu.bounce(1); gpu.bounce(2);
    auto previous = gpu.resolveLevel(0);
    for (unsigned level = 1; level <= 2; ++level) {
        const auto current = gpu.resolveLevel(level);
        const auto before = coveredMean(scene, previous), after = coveredMean(scene, current);
        MESSAGE("gpu bake sealed level=" << level << " before=" << before.x << " after=" << after.x
                << " ratio=" << after.x / before.x);
        CHECK(after.x > 0);
        for (int channel = 0; channel < 3; ++channel) {
            CHECK(after[channel] <= before[channel] * 0.55);
            float maximum = 0;
            for (const auto& face : previous)
                for (const auto value : face) maximum = std::max(maximum, value[channel]);
            for (const auto& face : current) for (const auto value : face) {
                CHECK(std::isfinite(value[channel]));
                CHECK(value[channel] >= 0.0f);
                CHECK(value[channel] <= maximum * 0.5f + 1e-5f);
            }
        }
        previous = current;
    }
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
}

TEST_CASE("render: gpu bake partial budgets and resumed traversals equal full steps") {
    auto* context = gpuContext();
    if (!context) return;
    context->validation().clear();
    const auto scene = cornerScene();
    BakerOptions options{.seed = 125, .threads = 1, .bounces = 2, .bounceRays = 128};
    Baker full(scene, options, gpuKernel(*context, scene, options));
    Baker partial(scene, options, gpuKernel(*context, scene, options));
    for (int pass = 0; pass < 2; ++pass) {
        full.step();
        while (!partial.step(7).passComplete) {}
    }
    for (unsigned level = 1; level <= 2; ++level) for (int pass = 0; pass < 2; ++pass) {
        full.bounce(level);
        while (!partial.bounce(level, 7).passComplete) {}
    }
    for (unsigned level = 0; level <= 2; ++level) {
        CHECK(full.resolveLevel(level) == partial.resolveLevel(level));
        for (std::size_t face = 0; face < scene.faces.size(); ++face)
            CHECK(full.buffers(level)[face].samples == partial.buffers(level)[face].samples);
    }
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
}

TEST_CASE("render: gpu bake resumes a bounce traversal after a deeper level started") {
    auto* context = gpuContext();
    if (!context) return;
    context->validation().clear();
    const auto scene = cornerScene();
    BakerOptions options{.seed = 41, .threads = 1, .bounces = 2, .bounceRays = 64};
    Baker ordered(scene, options, gpuKernel(*context, scene, options));
    Baker interleaved(scene, options, gpuKernel(*context, scene, options));
    ordered.step(); interleaved.step();
    ordered.bounce(1); ordered.bounce(2); ordered.bounce(1);
    interleaved.bounce(1); interleaved.bounce(2);
    // Level 1's second traversal stops partway, level 2 starts its own (uploading level 1's snapshot),
    // then level 1 resumes and must still read its own source, level 0.
    REQUIRE(!interleaved.bounce(1, 7).passComplete);
    interleaved.bounce(2, 7);
    while (!interleaved.bounce(1, 7).passComplete) {}
    CHECK(ordered.resolveLevel(1) == interleaved.resolveLevel(1));
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
}

TEST_CASE("render: gpu bake session converges with CPU coverage-weighted parity") {
    auto* context = gpuContext();
    if (!context) return;
    context->validation().clear();
    auto input = cornerScene();
    BakeSessionOptions options;
    options.baker = {.seed = 73, .threads = 1, .skyRays = 64, .bounces = 2, .bounceRays = 128};
    options.initialPasses = options.maxPasses = 2;
    BakeSession cpu(input, options);
    BakeSession gpu(input, options, 0, {}, [context](const auto& scene, const auto& settings) {
        return gpuKernel(*context, scene, settings);
    });
    REQUIRE(gpu.gpu());
    while (!cpu.progress().idle) cpu.advance(10000);
    while (!gpu.progress().idle) gpu.advance(10000);
    const auto a = coveredMean(input, cpu.baker().resolve());
    const auto b = coveredMean(input, gpu.baker().resolve());
    for (int channel = 0; channel < 3; ++channel)
        CHECK(b[channel] == doctest::Approx(a[channel]).epsilon(0.06));
    CHECK(context->validation().errorCount() == 0);
}
