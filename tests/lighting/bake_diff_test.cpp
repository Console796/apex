#include "lighting/bake_diff.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::lighting;

namespace {

// A 64-unit square facing +z, centred at (x, y, z).
BakeFace quad(std::uint32_t side, DVec3 centre) {
    BakeFace face;
    face.solidId = 7;
    face.sideId = side;
    face.vertices = {centre + DVec3{-32, -32, 0}, centre + DVec3{32, -32, 0}, centre + DVec3{32, 32, 0},
                     centre + DVec3{-32, 32, 0}};
    face.chart.dimensions = {5, 5};
    face.chart.origin = face.vertices[0];
    face.chart.luxelToWorld = {DVec3{16, 0, 0}, DVec3{0, 16, 0}};
    return face;
}

Light point(DVec3 origin, std::uint64_t id = 8, double radius = 500) {
    Light light;
    light.id = id;
    light.origin = origin;
    light.intensity = {10000, 10000, 10000};
    light.quadratic = 1;
    light.radius = radius;
    return light;
}

struct Scene {
    std::vector<BakeFace> faces;
    std::vector<Light> lights;
    std::vector<std::string> materials{"dev/dev_measuregeneric01"};
    BakeSceneView view(std::vector<FaceKey>& keys) {
        keys = faceKeys(faces);
        return {faces, lights, materials, keys};
    }
};

// Five faces along x, 300 apart, a point light over the first.
Scene row() {
    Scene scene;
    for (std::uint32_t i = 0; i < 5; ++i) scene.faces.push_back(quad(i + 1, {300.0 * i, 0, 0}));
    scene.lights = {point({0, 0, 50})};
    return scene;
}

BakeDiff diff(Scene& before, Scene& after) {
    std::vector<FaceKey> a, b;
    const auto old = before.view(a);
    return diffScenes(old, after.view(b));
}

std::vector<int> flagged(const std::vector<char>& flags) {
    std::vector<int> result;
    for (std::size_t i = 0; i < flags.size(); ++i) if (flags[i]) result.push_back(int(i));
    return result;
}

} // namespace

TEST_CASE("bake diff: identical scenes are clean") {
    auto a = row(), b = row();
    const auto result = diff(a, b);
    CHECK_FALSE(result.full);
    CHECK(result.dirtyCount == 0);
    CHECK(result.bounceDirtyCount == 0);
    CHECK(result.oldOfNew == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE("bake diff: a moved light dirties only faces in range of its old and new position") {
    auto a = row(), b = row();
    b.lights[0].origin = {1200, 0, 50};
    const auto result = diff(a, b);
    CHECK_FALSE(result.full);
    CHECK(flagged(result.dirty) == std::vector<int>{0, 1, 3, 4});
    // The middle face is out of range of both, but within bounce reach of its dirty neighbours.
    CHECK(flagged(result.bounceDirty) == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE("bake diff: light range and facing bound the dirty set") {
    auto a = row(), b = row();
    b.lights[0].intensity = {20000, 20000, 20000};
    SUBCASE("range") {
        b.lights[0].radius = 100; // Only the face under the light is reached, before and after (old 500).
        a.lights[0].radius = 100;
        const auto result = diff(a, b);
        CHECK(flagged(result.dirty) == std::vector<int>{0});
    }
    SUBCASE("a light behind the plane reaches nothing") {
        a.lights[0].origin = b.lights[0].origin = {0, 0, -50};
        CHECK(diff(a, b).dirtyCount == 0);
    }
    SUBCASE("a spot only dirties faces inside its cone") {
        for (auto* scene : {&a, &b}) {
            auto& light = scene->lights[0];
            light.kind = LightKind::Spot;
            light.origin = {600, 0, 100};
            light.direction = {0, 0, -1};
            light.stopdot = 0.99;
            light.stopdot2 = 0.95;
            light.radius = 2000;
        }
        const auto result = diff(a, b);
        CHECK(flagged(result.dirty) == std::vector<int>{2});
    }
}

TEST_CASE("bake diff: sun dirties faces that face it; hasDirect=false lights are ignored") {
    auto a = row(), b = row();
    Light sun;
    sun.kind = LightKind::Sun;
    sun.id = 16;
    sun.direction = {0, 0, -1};
    sun.intensity = {1, 1, 1};
    a.lights = b.lights = {sun};
    b.lights[0].direction = glm::normalize(DVec3{0.3, 0, -1});
    CHECK(diff(a, b).dirtyCount == 5);
    a.faces[1].normal = b.faces[1].normal = {0, 0, -1};
    CHECK(flagged(diff(a, b).dirty) == std::vector<int>{0, 2, 3, 4});
    auto c = row(), d = row();
    d.lights[0].origin = {1200, 0, 50};
    d.lights[0].directMode = c.lights[0].directMode = 0;
    CHECK(diff(c, d).dirtyCount == 0);
}

TEST_CASE("bake diff: a moved brush face dirties itself, its neighbourhood and its shadow") {
    auto a = row(), b = row();
    b.faces[2].vertices[0].z += 8; // Geometry hash changes; the chart is untouched.
    const auto result = diff(a, b);
    CHECK_FALSE(result.full);
    // Self plus both neighbours inside the near-field reach; the light is range-limited to the first two faces.
    CHECK(flagged(result.dirty) == std::vector<int>{1, 2, 3});
    CHECK(flagged(result.bounceDirty) == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE("bake diff: a changed occluder dirties faces in a light's shadow, not off-axis faces") {
    Scene a;
    a.faces = {quad(1, {0, 0, 0}), quad(2, {500, 0, 50}), quad(3, {1500, 0, 0}), quad(4, {500, 1500, 0})};
    Light far = point({0, 0, 100}, 8, 0);
    far.intensity = {1e6, 1e6, 1e6};
    a.lights = {far};
    auto b = a;
    b.faces[1].vertices[0].z += 4;
    const auto result = diff(a, b);
    CHECK(flagged(result.dirty) == std::vector<int>{1, 2});
}

TEST_CASE("bake diff: added and removed faces") {
    auto a = row(), b = row();
    b.faces.push_back(quad(9, {1500, 0, 0}));
    auto result = diff(a, b);
    CHECK(result.oldOfNew.back() == -1);
    CHECK(result.dirty.back() == 1);
    CHECK(flagged(result.dirty) == std::vector<int>{4, 5}); // The new face's neighbourhood.
    auto c = row(), d = row();
    d.faces.erase(d.faces.begin() + 2);
    result = diff(c, d);
    CHECK_FALSE(result.full);
    CHECK(flagged(result.dirty) == std::vector<int>{1, 2}); // Neighbours of the hole.
}

TEST_CASE("bake diff: unbounded or unusable changes fall back to a full restart") {
    auto a = row(), b = row();
    Light ambient;
    ambient.kind = LightKind::Ambient;
    ambient.id = 24;
    ambient.intensity = {1, 1, 1};
    a.lights.push_back(ambient);
    b.lights.push_back(ambient);
    CHECK_FALSE(diff(a, b).full);
    b.lights.back().intensity = {2, 2, 2};
    auto result = diff(a, b);
    CHECK(result.full);
    CHECK(result.dirtyCount == 5);
    SUBCASE("sky material") {
        a.materials.push_back("tools/toolsskybox");
        b.materials = a.materials;
        b.lights = a.lights;
        b.faces[0].material = 1;
        CHECK(diff(a, b).reason == "sky material changed");
    }
    SUBCASE("mostly new faces") {
        auto c = row(), d = row();
        for (auto& face : d.faces) face.solidId = 99;
        CHECK(diff(c, d).reason == "face set changed");
    }
}

TEST_CASE("bake diff: albedo change dirties the face as a bounce source") {
    auto a = row(), b = row();
    b.faces[4].albedo = Vec3{0.9f};
    const auto result = diff(a, b);
    CHECK(flagged(result.dirty) == std::vector<int>{4});
    CHECK(flagged(result.bounceDirty) == std::vector<int>{2, 3, 4});
}

TEST_CASE("bake diff: fragment ordinals identify subdivided sides") {
    std::vector<BakeFace> faces{quad(1, {0, 0, 0}), quad(1, {100, 0, 0}), quad(2, {200, 0, 0})};
    const auto keys = faceKeys(faces);
    CHECK(keys[0].fragment == 0);
    CHECK(keys[1].fragment == 1);
    CHECK(keys[2].fragment == 0);
    CHECK(keys[0] != keys[1]);
}
