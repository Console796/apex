#include "lighting/baker.hpp"

#include "document/document.hpp"
#include "geometry/solid_block.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace apex;
using namespace apex::lighting;

namespace {

BakeFace floorFace(DVec3 origin = {-64, -64, 0}, double size = 128) {
    const std::vector<DVec3> vertices{origin, origin + DVec3{size, 0, 0},
        origin + DVec3{size, size, 0}, origin + DVec3{0, size, 0}};
    geometry::Face face;
    face.vertices = {0, 1, 2, 3};
    face.plane = {{0, 0, 1}, origin.z};
    face.sideId = 42;
    auto result = bakeFaces(face, vertices);
    REQUIRE(result.size() == 1);
    return std::move(result[0]);
}

Light pointLight() {
    Light light;
    light.origin = {0, 0, 128};
    light.intensity = {30000, 20000, 10000}; // Keep values HDR (no clamping or tone mapping).
    light.quadratic = 1;
    return light;
}

std::vector<Light> environment() {
    Light sun;
    sun.kind = LightKind::Sun;
    sun.direction = {0, 0, -1};
    sun.intensity = {2, 3, 4};
    Light ambient;
    ambient.kind = LightKind::Ambient;
    ambient.intensity = {0.5, 0.75, 1};
    return {sun, ambient};
}

// One BLAS with per-primitive sky metadata also exercises mixed-material instances.
struct Casters {
    TriangleMesh mesh;
    std::vector<bool> sky;
    std::vector<std::optional<std::size_t>> faces;

    void quad(DVec3 a, DVec3 b, DVec3 c, DVec3 d, bool isSky = false,
              std::optional<std::size_t> face = {}) {
        const auto base = static_cast<std::uint32_t>(mesh.positions.size());
        for (const auto p : {a, b, c, d})
            mesh.positions.push_back({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        sky.insert(sky.end(), {isSky, isSky});
        faces.insert(faces.end(), {face, face});
    }

    void box(DVec3 lo, DVec3 hi, bool skyCeiling = false) {
        quad({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z});
        quad({lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, skyCeiling);
        quad({lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {lo.x, lo.y, hi.z});
        quad({hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z});
        quad({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z});
        quad({lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z});
    }

    BakeOcclusion build() const {
        auto blas = CpuBlas::build(mesh.positions, mesh.indices);
        REQUIRE(blas);
        const std::array<CpuInstance, 1> instances{{{*blas,
            {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, 7}}};
        auto accel = CpuAccel::build(instances);
        REQUIRE(accel);
        return {std::shared_ptr<const CpuAccel>(std::move(*accel)), [flags = sky](const RayHit& hit) {
            CHECK(hit.customIndex == 7);
            return flags.at(hit.primitive);
        }, [indices = faces](const RayHit& hit) { return indices.at(hit.primitive); }};
    }
};

void near(DVec3 actual, DVec3 expected, double tolerance = 1e-5) {
    for (int c = 0; c < 3; ++c)
        CHECK(actual[c] == doctest::Approx(expected[c]).epsilon(tolerance));
}

DVec3 fourPoint(const BakeFace& face, std::size_t index, std::span<const Light> lights) {
    DVec3 sum{0};
    const DVec2 sample{static_cast<double>(index % static_cast<std::size_t>(face.chart.dimensions[0])),
                      static_cast<double>(index / static_cast<std::size_t>(face.chart.dimensions[0]))};
    for (const double t : {-0.5, 0.5})
        for (const double s : {-0.5, 0.5})
            for (const auto& light : lights)
                sum += irradiance(light, clampToFace(face, face.chart.worldAt(sample + DVec2{s, t})) + face.normal,
                                  face.normal);
    return sum / 4.0;
}

std::size_t centre(const BakeFace& face) {
    return static_cast<std::size_t>(face.chart.dimensions[1] / 2 * face.chart.dimensions[0] +
                                    face.chart.dimensions[0] / 2);
}

BakeFace quadFace(DVec3 origin, DVec3 u, DVec3 v, DVec3 normal) {
    const std::vector<DVec3> vertices{origin, origin + 128.0 * u,
        origin + 128.0 * (u + v), origin + 128.0 * v};
    geometry::Face face;
    face.vertices = {0, 1, 2, 3};
    face.plane = {normal, glm::dot(normal, origin)};
    face.uv.uaxis = u;
    face.uv.vaxis = v;
    face.uv.shiftU = 7;
    face.uv.shiftV = -11; // Exercise nonzero chart offsets and mins in hit-to-luxel lookup.
    auto result = bakeFaces(face, vertices);
    REQUIRE(result.size() == 1);
    return std::move(result[0]);
}

Casters faceCasters(const BakeScene& scene) {
    Casters result;
    for (std::size_t i = 0; i < scene.faces.size(); ++i) {
        const auto& v = scene.faces[i].vertices;
        REQUIRE(v.size() == 4);
        result.quad(v[0], v[1], v[2], v[3], false, i);
    }
    return result;
}

BakeScene bounceCorner(Vec3 albedo = Vec3{0.5f}, bool blocked = false) {
    BakeScene scene;
    scene.faces = {quadFace({-64, -64, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}),
                   quadFace({64, -64, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0})};
    scene.faces[0].albedo = albedo;
    auto light = pointLight();
    light.kind = LightKind::Spot;
    light.origin = {-32, 0, 96};
    light.direction = {0, 0, -1};
    light.intensity = DVec3{30000};
    light.stopdot = 0.98;
    light.stopdot2 = 0.95; // The floor is lit; every wall sample lies outside the cone.
    scene.lights = {light};
    auto casters = faceCasters(scene);
    if (blocked)
        casters.quad({16, -1e4, -32}, {16, 1e4, -32}, {16, 1e4, 160}, {16, -1e4, 160});
    scene.occlusion = casters.build();
    return scene;
}

DVec3 mean(const std::vector<std::vector<Vec3>>& level) {
    DVec3 sum{0};
    std::size_t count = 0;
    for (const auto& face : level)
        for (const auto value : face) {
            sum += DVec3(value);
            ++count;
        }
    return sum / static_cast<double>(count);
}

// Mean weighted by each luxel's footprint on its face (axis-aligned quads only), so partial edge
// luxels count for the area they light: the form in which bounce energy must not exceed albedo.
DVec3 coveredMean(const BakeScene& scene, const std::vector<std::vector<Vec3>>& level) {
    constexpr int kSub = 8;
    DVec3 sum{0};
    double weight = 0;
    for (std::size_t f = 0; f < level.size(); ++f) {
        const auto& face = scene.faces[f];
        DVec3 lo{std::numeric_limits<double>::infinity()}, hi{-std::numeric_limits<double>::infinity()};
        for (const auto& v : face.vertices) {
            lo = glm::min(lo, v - 1e-6);
            hi = glm::max(hi, v + 1e-6);
        }
        const auto width = static_cast<std::size_t>(face.chart.dimensions[0]);
        for (std::size_t i = 0; i < level[f].size(); ++i) {
            int inside = 0;
            for (int a = 0; a < kSub; ++a)
                for (int b = 0; b < kSub; ++b) {
                    const DVec3 p = face.chart.worldAt(DVec2(double(i % width) + (a + 0.5) / kSub - 0.5,
                                                             double(i / width) + (b + 0.5) / kSub - 0.5));
                    inside += glm::all(glm::greaterThanEqual(p, lo)) && glm::all(glm::lessThanEqual(p, hi));
                }
            const double covered = double(inside) / (kSub * kSub);
            sum += DVec3(level[f][i]) * covered;
            weight += covered;
        }
    }
    return sum / weight;
}

} // namespace

TEST_CASE("baker: pass zero matches lifted VRAD four-point irradiance") {
    BakeScene scene;
    scene.faces = {floorFace()};
    scene.lights = {pointLight()};
    Baker baker(scene);
    CHECK(baker.resolve()[0][0] == Vec3{0});
    const auto step = baker.step();
    CHECK(step.passComplete);
    CHECK(step.luxels == 81);
    const auto result = baker.resolve()[0];
    for (std::size_t i = 0; i < result.size(); ++i) {
        near(DVec3(result[i]), fourPoint(scene.faces[0], i, scene.lights));
        CHECK(baker.buffers()[0].samples[i] == 4);
    }
    CHECK(result[centre(scene.faces[0])].x > 1.0f);
}

TEST_CASE("baker: a box shadows point light without changing exposed luxels") {
    BakeScene scene;
    scene.faces = {floorFace()};
    scene.lights = {pointLight()};
    Baker open(scene);
    open.step();
    Casters casters;
    casters.box({-16, -16, 48}, {16, 16, 64});
    scene.occlusion = casters.build();
    Baker shadow(scene);
    shadow.step();
    const auto before = open.resolve()[0], after = shadow.resolve()[0];
    CHECK(after[centre(scene.faces[0])] == Vec3{0});
    near(DVec3(after[0]), DVec3(before[0]));
    near(DVec3(after[8]), DVec3(before[8]));
    CHECK(before[centre(scene.faces[0])].x > 0.0f);
}

TEST_CASE("baker: open sky, solid roof, sealed room and sky-material first hit") {
    BakeScene scene;
    scene.faces = {floorFace()};
    scene.lights = environment();
    const auto index = centre(scene.faces[0]);
    SUBCASE("open sky is exactly sun plus skyAmbientResponse") {
        // Include the receiving floor in the BVH to verify the unit lift avoids self-shadowing.
        Casters casters;
        casters.quad({-1e6, -1e6, 0}, {1e6, -1e6, 0}, {1e6, 1e6, 0}, {-1e6, 1e6, 0});
        scene.occlusion = casters.build();
        Baker baker(scene);
        baker.step();
        near(DVec3(baker.resolve()[0][index]), scene.lights[0].intensity +
             scene.lights[1].intensity * skyAmbientResponse(1));
    }
    SUBCASE("finite roof blocks sun but leaves some ambient through open sides") {
        Casters casters;
        casters.quad({-16, -16, 32}, {16, -16, 32}, {16, 16, 32}, {-16, 16, 32});
        scene.occlusion = casters.build();
        Baker baker(scene, {.seed = 12});
        baker.step();
        const auto value = baker.resolve()[0][index];
        CHECK(value.x > 0.0f);
        CHECK(value.x < scene.lights[1].intensity.x);
        near(DVec3(value), scene.lights[1].intensity * (value.x / scene.lights[1].intensity.x));
    }
    SUBCASE("sealed room sees no sky") {
        Casters casters;
        casters.box({-128, -128, -8}, {128, 128, 64});
        scene.occlusion = casters.build();
        Baker baker(scene);
        baker.step();
        for (const auto value : baker.resolve()[0]) CHECK(value == Vec3{0});
    }
    SUBCASE("sky ceiling counts as sky even with solid geometry behind it") {
        Casters casters;
        casters.box({-1e7, -1e7, -8}, {1e7, 1e7, 64}, true);
        casters.quad({-1e7, -1e7, 80}, {1e7, -1e7, 80}, {1e7, 1e7, 80}, {-1e7, 1e7, 80});
        scene.occlusion = casters.build();
        Baker baker(scene);
        baker.step();
        near(DVec3(baker.resolve()[0][index]), scene.lights[0].intensity + scene.lights[1].intensity);
    }
}

TEST_CASE("baker: direct modes and spots use the shared light model") {
    BakeScene scene;
    scene.faces = {floorFace()};
    auto spot = pointLight();
    spot.kind = LightKind::Spot;
    spot.direction = {0, 0, -1};
    spot.stopdot = 0.95;
    spot.stopdot2 = 0.75;
    spot.exponent = 2;
    scene.lights = {spot};
    Baker enabled(scene);
    enabled.step();
    for (std::size_t i = 0; i < enabled.buffers()[0].samples.size(); ++i)
        near(DVec3(enabled.resolve()[0][i]), fourPoint(scene.faces[0], i, scene.lights));
    scene.lights[0].directMode = 0;
    Baker disabled(scene);
    disabled.step();
    for (const auto value : disabled.resolve()[0]) CHECK(value == Vec3{0});
}

TEST_CASE("baker: progressive direct means and seeds are independent of threads and budgets") {
    BakeScene scene;
    scene.faces = {floorFace(), floorFace({128, -64, 0})};
    scene.lights = {pointLight()};
    Baker direct(scene);
    direct.step();
    const auto first = direct.resolve();
    for (int pass = 1; pass < 32; ++pass) direct.step();
    for (std::size_t f = 0; f < first.size(); ++f)
        for (std::size_t i = 0; i < first[f].size(); ++i) {
            near(DVec3(direct.resolve()[f][i]), DVec3(first[f][i]));
            CHECK(direct.buffers()[f].samples[i] == 128);
        }

    Casters casters;
    // Half the hemisphere at the centre is covered, giving an analytic ambient mean of 1/2.
    casters.quad({0, -1e6, 1.001}, {1e6, -1e6, 1.001}, {1e6, 1e6, 1.001}, {0, 1e6, 1.001});
    scene.occlusion = casters.build();
    scene.lights = {environment()[1]};
    Baker serial(scene, {.seed = 123, .threads = 1});
    Baker parallel(scene, {.seed = 123, .threads = 3});
    const auto index = centre(scene.faces[0]);
    serial.step();
    for (int pass = 0; pass < 64; ++pass) {
        if (pass > 0) serial.step();
        while (!parallel.step(7).passComplete) {}
    }
    CHECK(serial.resolve() == parallel.resolve());
    near(DVec3(serial.resolve()[0][index]), scene.lights[0].intensity * 0.5, 0.01);
}

TEST_CASE("baker: subset passes touch only selected faces and resume within budget") {
    BakeScene scene;
    scene.faces = {floorFace(), floorFace({128, -64, 0})};
    scene.lights = {pointLight()};
    Baker baker(scene);
    const std::array<std::size_t, 1> subset{1};
    CHECK(baker.step(0, subset).luxels == 0);
    const auto partial = baker.step(3, subset);
    CHECK(partial.luxels == 3);
    CHECK_FALSE(partial.passComplete);
    CHECK(std::count(baker.buffers()[1].samples.begin(), baker.buffers()[1].samples.end(), 4) == 3);
    const auto rest = baker.step(1000, subset);
    CHECK(rest.luxels == 78);
    CHECK(rest.passComplete);
    for (const auto count : baker.buffers()[0].samples) CHECK(count == 0);
    for (const auto value : baker.resolve()[0]) CHECK(value == Vec3{0});
    for (const auto count : baker.buffers()[1].samples) CHECK(count == 4);
    const std::array<std::size_t, 2> duplicate{1, 1};
    const std::array<std::size_t, 1> invalid{2};
    CHECK_THROWS_AS(baker.step(1, duplicate), std::invalid_argument);
    CHECK_THROWS_AS(baker.step(1, invalid), std::invalid_argument);
    baker.step(1000, subset);
    for (const auto count : baker.buffers()[1].samples) CHECK(count == 8);
    baker.step();
    for (const auto count : baker.buffers()[0].samples) CHECK(count == 4);
    for (const auto count : baker.buffers()[1].samples) CHECK(count == 12);
    Baker empty({});
    CHECK(empty.step().passComplete);
    CHECK(empty.resolve().empty());
}

TEST_CASE("baker: bounce is opt-in and direct accumulators stay bit-identical") {
    const auto scene = bounceCorner();
    Baker direct(scene);
    Baker enabled(scene, {.bounces = 1});
    for (int pass = 0; pass < 3; ++pass) {
        direct.step();
        enabled.step();
    }
    CHECK(direct.resolve() == enabled.resolve());
    CHECK(direct.resolve() == direct.resolveLevel(0));
    enabled.bounce(1);
    CHECK(direct.resolve() == enabled.resolveLevel(0));
    for (std::size_t f = 0; f < scene.faces.size(); ++f) {
        CHECK(direct.buffers()[f].radiance == enabled.buffers()[f].radiance);
        CHECK(direct.buffers()[f].samples == enabled.buffers()[f].samples);
    }
    CHECK_THROWS_AS(direct.bounce(1), std::out_of_range);
    CHECK_THROWS_AS(enabled.bounce(0), std::out_of_range);
    CHECK_THROWS_AS(enabled.resolveLevel(2), std::out_of_range);
}

TEST_CASE("baker: floor reflectivity scales and tints bounce onto an unlit wall") {
    const auto bake = [](Vec3 albedo) {
        Baker baker(bounceCorner(albedo), {.seed = 73, .bounces = 1, .bounceRays = 512});
        baker.step();
        for (const auto value : baker.resolveLevel(0)[1]) CHECK(value == Vec3{0});
        baker.bounce(1);
        return baker.resolveLevel(1)[1];
    };
    const auto black = bake(Vec3{0});
    const auto quarter = bake(Vec3{0.25f});
    const auto half = bake(Vec3{0.5f});
    const auto red = bake({0.5f, 0, 0});
    CHECK(std::ranges::any_of(half, [](Vec3 v) { return v.x > 0; }));
    for (std::size_t i = 0; i < half.size(); ++i) {
        CHECK(black[i] == Vec3{0});
        CHECK(half[i] == quarter[i] * 2.0f);
        CHECK(red[i] == Vec3{half[i].x, 0, 0});
    }
}

TEST_CASE("baker: bounce respects first-hit occlusion, sky and unmapped geometry") {
    auto scene = bounceCorner();
    SUBCASE("unmapped occluder blocks floor to wall transport") { scene = bounceCorner(Vec3{0.5f}, true); }
    SUBCASE("missing face callback contributes zero") { scene.occlusion.faceOf = {}; }
    SUBCASE("unmapped hits contribute zero") {
        scene.occlusion.faceOf = [](const RayHit&) { return std::optional<std::size_t>{}; };
    }
    SUBCASE("sky takes precedence over bake face mapping") {
        scene.occlusion.isSky = [](const RayHit&) { return true; };
    }
    SUBCASE("no acceleration means no bounce") { scene.occlusion.accel.reset(); }
    Baker baker(scene, {.bounces = 1, .bounceRays = 256});
    baker.step();
    baker.bounce(1);
    for (const auto value : baker.resolveLevel(1)[1]) CHECK(value == Vec3{0});
}

TEST_CASE("baker: bounce snapshots and seeds agree across threads and budgets") {
    const auto scene = bounceCorner();
    Baker serial(scene, {.seed = 125, .threads = 1, .bounces = 2, .bounceRays = 128});
    Baker parallel(scene, {.seed = 125, .threads = 3, .bounces = 2, .bounceRays = 128});
    serial.step();
    while (!parallel.step(7).passComplete) {}
    for (unsigned level = 1; level <= 2; ++level) {
        for (int pass = 0; pass < 3; ++pass) {
            serial.bounce(level);
            while (!parallel.bounce(level, 7).passComplete) {}
        }
        CHECK(serial.resolveLevel(level) == parallel.resolveLevel(level));
    }
    CHECK(serial.resolve() == parallel.resolve());
    const auto direct = serial.resolveLevel(0), first = serial.resolveLevel(1), second = serial.resolveLevel(2);
    const auto total = serial.resolve();
    for (std::size_t f = 0; f < total.size(); ++f)
        for (std::size_t i = 0; i < total[f].size(); ++i)
            CHECK(total[f][i] == direct[f][i] + first[f][i] + second[f][i]);

    // Mutating the upstream buffer between budget chunks must not alter this traversal's source.
    Baker frozen(scene, {.seed = 125, .bounces = 1});
    Baker updated(scene, {.seed = 125, .bounces = 1});
    const std::array<std::size_t, 1> floor{0}, wall{1};
    frozen.step(40, floor);
    updated.step(40, floor);
    updated.bounce(1, 3, wall);
    updated.step(1000, floor);
    while (!updated.bounce(1, 7, wall).passComplete) {}
    frozen.bounce(1, 1000, wall);
    CHECK(frozen.resolveLevel(1) == updated.resolveLevel(1));
    updated.bounce(1, 1000, wall);
    CHECK(mean(updated.resolveLevel(1)).x > mean(frozen.resolveLevel(1)).x);
}

TEST_CASE("baker: external kernel receives stable traversal snapshots and sample counts") {
    struct Kernel final : BakeKernel {
        std::vector<bool> starts;
        void run(unsigned level, std::span<const BakeWork> work,
                 const std::vector<std::vector<Vec3>>* source, bool newTraversal,
                 std::span<Vec3> sums) override {
            starts.push_back(newTraversal);
            CHECK((source != nullptr) == (level != 0));
            std::size_t output = 0;
            for (const auto& item : work) {
                for (auto luxel = item.begin; luxel < item.end; ++luxel) {
                    const auto prior = static_cast<float>(item.samples[luxel]);
                    sums[output++] = Vec3{4.0f + prior + static_cast<float>(level)};
                }
            }
            CHECK(output == sums.size());
        }
    };
    BakeScene scene;
    scene.faces.push_back(floorFace());
    auto fullKernel = std::make_shared<Kernel>();
    auto partialKernel = std::make_shared<Kernel>();
    Baker full(scene, {.bounces = 1}, fullKernel);
    Baker partial(scene, {.bounces = 1}, partialKernel);
    for (unsigned level = 0; level <= 1; ++level) {
        for (int pass = 0; pass < 2; ++pass) {
            if (level == 0) {
                full.step();
                while (!partial.step(7).passComplete) {}
            } else {
                full.bounce(level);
                while (!partial.bounce(level, 7).passComplete) {}
            }
        }
        CHECK(full.resolveLevel(level) == partial.resolveLevel(level));
        CHECK(full.buffers(level)[0].samples == partial.buffers(level)[0].samples);
    }
    CHECK(std::count(partialKernel->starts.begin(), partialKernel->starts.end(), true) == 3);
    CHECK(partialKernel->starts.size() > fullKernel->starts.size());
}

TEST_CASE("baker: sample points outside the face move just inside it") {
    const auto face = quadFace({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1});
    const DVec3 interior{40, 50, 0};
    CHECK(clampToFace(face, interior) == interior);
    const auto past = clampToFace(face, {-8, 64, 0}); // Beyond the x = 0 edge, as a crease's edge luxel corner.
    CHECK(past.x == doctest::Approx(1));
    CHECK(past.y == doctest::Approx(64).epsilon(0.01));
    CHECK(past.z == doctest::Approx(0));
    const auto corner = clampToFace(face, {-8, -8, 0}); // Outside a vertex: towards the centre.
    CHECK(corner.x > 0);
    CHECK(corner.y > 0);
    CHECK(glm::length(corner) == doctest::Approx(1));
}

TEST_CASE("baker: bounce reflectivity scales bounce transport only") {
    BakeScene scene;
    scene.faces = {
        quadFace({-64, -64, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}),
        quadFace({-64, -64, 128}, {1, 0, 0}, {0, 1, 0}, {0, 0, -1})};
    auto light = pointLight();
    light.origin = {0, 0, 64};
    scene.lights = {light};
    scene.occlusion = faceCasters(scene).build();
    const auto level = [&](double reflectivity, unsigned index) {
        Baker baker(scene, {.seed = 5, .bounces = 1, .bounceRays = 64, .bounceReflectivity = reflectivity});
        baker.step();
        baker.bounce(1);
        return mean(baker.resolveLevel(index));
    };
    near(level(0.5, 0), level(1, 0));
    near(level(0.5, 1), level(1, 1) * 0.5);
    CHECK_THROWS_AS(Baker(scene, {.bounceReflectivity = 1.5}), std::invalid_argument);
}

TEST_CASE("baker: sealed-room successive bounce energy decreases with reflectivity") {
    BakeScene scene;
    scene.faces = {
        quadFace({-64, -64, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}),
        quadFace({-64, -64, 128}, {1, 0, 0}, {0, 1, 0}, {0, 0, -1}),
        quadFace({-64, -64, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}),
        quadFace({64, -64, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}),
        quadFace({-64, -64, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}),
        quadFace({-64, 64, 0}, {1, 0, 0}, {0, 0, 1}, {0, -1, 0})};
    constexpr float albedo = 0.5f;
    for (auto& face : scene.faces) face.albedo = Vec3{albedo};
    auto light = pointLight();
    light.origin = {0, 0, 64};
    scene.lights = {light};
    scene.occlusion = faceCasters(scene).build();
    Baker baker(scene, {.seed = 911, .bounces = 2, .bounceRays = 512});
    baker.step();
    baker.bounce(1);
    baker.bounce(2);
    auto previous = baker.resolveLevel(0);
    for (unsigned level = 1; level <= 2; ++level) {
        const auto current = baker.resolveLevel(level);
        const auto before = coveredMean(scene, previous), after = coveredMean(scene, current);
        CHECK(after.x > 0);
        for (int c = 0; c < 3; ++c) {
            CHECK(after[c] <= before[c] * albedo * 1.1); // Finite chart/sample boundary noise.
            float maximum = 0;
            for (const auto& face : previous)
                for (const auto value : face) maximum = std::max(maximum, value[c]);
            for (const auto& face : current)
                for (const auto value : face) {
                    CHECK(std::isfinite(value[c]));
                    CHECK(value[c] >= 0.0f);
                    CHECK(value[c] <= maximum * albedo + 1e-5f);
                }
        }
        previous = current;
    }
}

TEST_CASE("baker: bounce subset passes touch only selected faces and resume within budget") {
    Baker baker(bounceCorner(), {.bounces = 1});
    baker.step();
    const std::array<std::size_t, 1> subset{1};
    CHECK(baker.bounce(1, 0, subset).luxels == 0);
    CHECK(baker.bounce(1, 3, subset).luxels == 3);
    CHECK(std::count(baker.buffers(1)[1].samples.begin(), baker.buffers(1)[1].samples.end(), 4) == 3);
    const auto rest = baker.bounce(1, 1000, subset);
    CHECK(rest.passComplete);
    CHECK(rest.luxels == baker.buffers(1)[1].samples.size() - 3);
    for (const auto count : baker.buffers(1)[0].samples) CHECK(count == 0);
    for (const auto value : baker.resolveLevel(1)[0]) CHECK(value == Vec3{0});
    for (const auto count : baker.buffers(1)[1].samples) CHECK(count == 4);
    const std::array<std::size_t, 2> duplicate{1, 1};
    const std::array<std::size_t, 1> invalid{2};
    CHECK_THROWS_AS(baker.bounce(1, 1, duplicate), std::invalid_argument);
    CHECK_THROWS_AS(baker.bounce(1, 1, invalid), std::invalid_argument);
    baker.bounce(1, 1000, subset);
    for (const auto count : baker.buffers(1)[1].samples) CHECK(count == 8);
    baker.bounce(1);
    for (const auto count : baker.buffers(1)[0].samples) CHECK(count == 4);
    for (const auto count : baker.buffers(1)[1].samples) CHECK(count == 12);
    Baker empty({}, {.bounces = 1});
    CHECK(empty.bounce(1).passComplete);
    CHECK(empty.resolveLevel(1).empty());
}

TEST_CASE("baker: bounce validates reflectivity and ray counts") {
    auto scene = bounceCorner();
    CHECK_THROWS_AS(Baker(scene, {.bounces = 1, .bounceRays = 0}), std::invalid_argument);
    for (const auto invalid : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()}) {
        scene.faces[0].albedo.x = invalid;
        CHECK_THROWS_AS(Baker(scene, {.bounces = 1}), std::invalid_argument);
    }
}

TEST_CASE("baker: document adapter charts brush sides and filters compile materials and displacement") {
    auto file = vmf::parse("world { \"id\" \"1\" \"classname\" \"worldspawn\" }");
    REQUIRE(file);
    document::Document document(*file);
    const std::array<geometry::PlaneSide, 6> planes{{
        {{{}, "", {}, 16, 0}, {{1, 0, 0}, 64}}, {{{}, "", {}, 16, 0}, {{-1, 0, 0}, 64}},
        {{{}, "", {}, 16, 0}, {{0, 1, 0}, 64}}, {{{}, "", {}, 16, 0}, {{0, -1, 0}, 64}},
        {{{}, "", {}, 16, 0}, {{0, 0, 1}, 0}}, {{{}, "", {}, 16, 0}, {{0, 0, -1}, 16}}}};
    auto poly = geometry::ConvexPolyhedron::fromPlanes(planes);
    REQUIRE(poly);
    auto block = geometry::solidBlock(*poly, {"brick/test"});
    const std::array<std::string, 6> names{"TOOLS/TOOLSSKYBOX2D", "tools/toolsnodraw", "custom/nolight",
                                         "brick/displacement", "brick/test", "brick/test"};
    auto sides = block.findAll("side");
    REQUIRE(sides.size() == names.size());
    for (std::size_t i = 0; i < sides.size(); ++i)
        *sides[i]->find("material") = kv1::Node::value("material", names[i]);
    sides[3]->append(kv1::Node::block("dispinfo")).value();
    auto tx = document.begin("bake input");
    const auto solid = tx.create(std::move(block), document.handle(document.roots()[0]));
    auto light = kv1::Node::block("entity");
    light.append(kv1::Node::value("classname", "light_environment")).value();
    light.append(kv1::Node::value("_light", "255 255 255 255")).value();
    light.append(kv1::Node::value("_ambient", "128 128 128 255")).value();
    tx.create(std::move(light));
    tx.commit();
    const auto scene = bakeSceneFromDocument(document, {}, [](std::string_view name) {
        BakeMaterial result;
        result.noLight = name == "custom/nolight";
        result.albedo = {0.2f, 0.3f, 0.4f};
        return result;
    });
    REQUIRE(scene);
    REQUIRE(scene->faces.size() == 2);
    CHECK(scene->materials == std::vector<std::string>{"brick/test"});
    CHECK(scene->lights.size() == 2);
    for (const auto& face : scene->faces) {
        CHECK(face.solidId == document.get(solid)->id);
        CHECK(face.sideId != 0);
        CHECK(face.albedo == Vec3{0.2f, 0.3f, 0.4f});
        CHECK(face.material == 0);
        CHECK(face.chart.dimensions == std::array<int, 2>{9, 9});
    }
    Baker baker(*scene);
    CHECK(baker.step().passComplete);
}

TEST_CASE("baker: bounce source changes only after upstream accumulation") {
    struct Kernel final : BakeKernel {
        std::vector<bool> changes;
        void run(unsigned level, std::span<const BakeWork>, const std::vector<std::vector<Vec3>>*,
                 bool changed, std::span<Vec3> sums) override {
            if (level == 1) changes.push_back(changed);
            std::ranges::fill(sums, Vec3{1});
        }
    };
    BakeScene scene;
    scene.faces = {floorFace(), floorFace()};
    auto kernel = std::make_shared<Kernel>();
    Baker baker(scene, {.threads = 1, .bounces = 1}, kernel);
    const std::array<std::size_t, 1> first{0}, second{1};
    baker.step();
    baker.bounce(1, 1, first);
    while (!baker.bounce(1, 7, first).passComplete) {}
    baker.bounce(1, 10000, second);
    baker.step(1, first);
    baker.bounce(1, 10000, first);
    REQUIRE(kernel->changes.size() > 3);
    CHECK(kernel->changes.front());
    CHECK(kernel->changes.back());
    CHECK(std::count(kernel->changes.begin(), kernel->changes.end(), true) == 2);
}

TEST_CASE("baker: interleaved face selections preserve CPU results") {
    BakeScene scene;
    scene.faces = {floorFace(), floorFace(), floorFace()};
    Light light;
    light.kind = LightKind::Ambient;
    light.intensity = {1, 1, 1};
    scene.lights.push_back(light);
    BakerOptions settings{.seed = 17, .threads = 1, .skyRays = 1, .bounces = 1, .bounceRays = 1};
    Baker full(scene, settings), selected(scene, settings);
    const std::array<std::size_t, 1> first{0};
    const std::array<std::size_t, 2> rest{1, 2};
    for (unsigned level = 0; level <= 1; ++level) {
        for (int pass = 0; pass < 2; ++pass) {
            if (!level) {
                full.step();
                while (!selected.step(7, first).passComplete) {}
                while (!selected.step(7, rest).passComplete) {}
            } else {
                full.bounce(level);
                while (!selected.bounce(level, 7, first).passComplete) {}
                while (!selected.bounce(level, 7, rest).passComplete) {}
            }
        }
        CHECK(full.resolveLevel(level) == selected.resolveLevel(level));
    }
    for (std::size_t face = 0; face < scene.faces.size(); ++face)
        CHECK(selected.resolveFace(face) == selected.resolve()[face]);
}
