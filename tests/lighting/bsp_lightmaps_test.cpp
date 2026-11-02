#include <doctest/doctest.h>
#include "../editor/fixtures.hpp"
#include "lighting/bsp_lightmaps.hpp"
#include "render/lightmap_atlas.hpp"

#include <algorithm>

using namespace apex;
namespace {

// A 64x64 top face as vbsp would store it: clockwise about +z, scale-16 lightmap vectors, 5x5 luxels.
lighting::BspFragment topFragment(std::uint16_t id = 0) {
    lighting::BspFragment fragment;
    fragment.hammerId = id;
    fragment.vertices = {{0, 0, 64}, {0, 64, 64}, {64, 64, 64}, {64, 0, 64}};
    fragment.plane = {{0, 0, 1}, 64};
    fragment.lightmapVectors = {DVec4(1.0 / 16, 0, 0, 0), DVec4(0, 1.0 / 16, 0, 0)};
    fragment.mins = {0, 0};
    fragment.extents = {4, 4};
    for (int i = 0; i < 25; ++i) fragment.luxels.push_back(Vec3(float(i)));
    return fragment;
}

// The id the document gave the +z side of the box.
std::uint16_t topId(const lighting::SideOwners& owners) {
    for (const auto& side : owners)
        if (side.plane.normal.z > 0.5) return std::uint16_t(side.sideId);
    FAIL("no +z side");
    return 0;
}

std::unique_ptr<document::Document> boxDocument() {
    auto doc = std::make_unique<document::Document>(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor_test::addBox(*doc, {0, 0, 0}, {64, 64, 64});
    return doc;
}
}

TEST_CASE("a BSP fragment keeps its exact VRAD lattice on the document side it came from") {
    auto doc = boxDocument();
    const auto owners = lighting::sideOwners(*doc);
    REQUIRE(owners.size() == 6);
    const auto kTop = topId(owners);
    const std::vector fragments{topFragment()}; // The lump names no side: found by plane and containment
    const auto mapped = lighting::mapBspFragments(fragments, owners);
    REQUIRE(mapped.faces.size() == 1);
    CHECK(mapped.stats.mapped == 1);
    CHECK(mapped.stats.faces == 1);
    const auto& face = mapped.faces[0];
    CHECK(face.sideId == kTop);
    CHECK(face.solidId == owners.front().solidId);
    CHECK(face.chart.dimensions == std::array<int, 2>{5, 5});
    CHECK(mapped.radiance[0].size() == 25);
    CHECK(face.normal.z == doctest::Approx(1));
    // Luxel (x, y) sits at stored mins + (x, y) in luxel space: world (16x, 16y, 64).
    const auto world = face.chart.worldAt({2, 3});
    CHECK(world.x == doctest::Approx(32));
    CHECK(world.y == doctest::Approx(48));
    CHECK(world.z == doctest::Approx(64));
    // The loop was flipped to counter-clockwise about the outward normal.
    CHECK(glm::dot(glm::cross(face.vertices[1] - face.vertices[0], face.vertices[2] - face.vertices[1]),
                   DVec3{0, 0, 1}) > 0);
}

TEST_CASE("fragments without light or a side stay unmapped") {
    auto doc = boxDocument();
    const auto owners = lighting::sideOwners(*doc);
    std::vector<lighting::BspFragment> fragments;
    fragments.push_back(topFragment(999));                    // lump names a side that is not there
    auto outside = topFragment();
    for (auto& v : outside.vertices) v.x += 100;
    fragments.push_back(outside);                             // not inside any side
    auto displacement = topFragment();
    displacement.displacement = true;
    fragments.push_back(displacement);
    auto dark = topFragment();
    dark.luxels.clear();
    fragments.push_back(dark);                                // lightOffset < 0
    auto wrongWay = topFragment();
    wrongWay.plane = {{0, 0, -1}, -64};
    fragments.push_back(wrongWay);                            // plane disagrees with every side at z = 64
    const auto rest = lighting::mapBspFragments(fragments, owners);
    CHECK(rest.faces.empty());
    CHECK(rest.stats.fragments == 5);
    CHECK(rest.stats.unowned == 3);
    CHECK(rest.stats.displacement == 1);
    CHECK(rest.stats.noData == 1);
}

TEST_CASE("a side vbsp split into several fragments maps each one to the same side") {
    auto doc = boxDocument();
    const auto owners = lighting::sideOwners(*doc);
    const auto kTop = topId(owners);
    auto left = topFragment();
    left.vertices = {{0, 0, 64}, {0, 64, 64}, {32, 64, 64}, {32, 0, 64}};
    left.extents = {2, 4};
    left.luxels.resize(15);
    auto right = topFragment();
    right.vertices = {{32, 0, 64}, {32, 64, 64}, {64, 64, 64}, {64, 0, 64}};
    right.mins = {2, 0};
    right.extents = {2, 4};
    right.luxels.resize(15);
    const std::vector fragments{left, right};
    const auto mapped = lighting::mapBspFragments(fragments, owners);
    REQUIRE(mapped.faces.size() == 2);
    CHECK(mapped.stats.faces == 1);
    auto atlas = render::LightmapAtlas::pack(mapped.faces);
    REQUIRE(atlas);
    atlas->compiled = true;
    CHECK(atlas->fragments.at({mapped.faces[0].solidId, kTop}).size() == 2);
    // Luxel centres are on VRAD's lattice: the right fragment's first luxel is world x = 32.
    CHECK(mapped.faces[1].chart.worldAt({0, 0}).x == doctest::Approx(32));
}

TEST_CASE("a merged fragment is cut to each side it covers, and coplanar overlap shares the light") {
    auto doc = std::make_unique<document::Document>(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor_test::addBox(*doc, {0, 0, 0}, {64, 64, 64});
    editor_test::addBox(*doc, {64, 0, 0}, {128, 64, 64});
    const auto owners = lighting::sideOwners(*doc);
    auto merged = topFragment();
    merged.vertices = {{0, 0, 64}, {0, 64, 64}, {128, 64, 64}, {128, 0, 64}}; // vbsp merged both tops
    merged.extents = {8, 4};
    merged.luxels.resize(45);
    const std::vector fragments{merged};
    const auto mapped = lighting::mapBspFragments(fragments, owners);
    CHECK(mapped.stats.mapped == 1);
    CHECK(mapped.stats.shared == 1);
    CHECK(mapped.stats.faces == 2);
    REQUIRE(mapped.faces.size() == 2);
    CHECK(mapped.faces[0].solidId != mapped.faces[1].solidId);
    CHECK(mapped.faces[0].chart.worldAt({3, 0}).x == doctest::Approx(mapped.faces[1].chart.worldAt({3, 0}).x));
    for (const auto& face : mapped.faces) {
        double minX = 1e9, maxX = -1e9;
        for (const auto& v : face.vertices) { minX = std::min(minX, v.x); maxX = std::max(maxX, v.x); }
        CHECK(maxX - minX == doctest::Approx(64));
    }
}
