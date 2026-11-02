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

    void quad(DVec3 a, DVec3 b, DVec3 c, DVec3 d, bool isSky = false) {
        const auto base = static_cast<std::uint32_t>(mesh.positions.size());
        for (const auto p : {a, b, c, d})
            mesh.positions.push_back({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        sky.insert(sky.end(), {isSky, isSky});
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
        }};
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
                sum += irradiance(light, face.chart.worldAt(sample + DVec2{s, t}) + face.normal, face.normal);
    return sum / 4.0;
}

std::size_t centre(const BakeFace& face) {
    return static_cast<std::size_t>(face.chart.dimensions[1] / 2 * face.chart.dimensions[0] +
                                    face.chart.dimensions[0] / 2);
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
