// Plan 0.8 D6 (0.8.4): Sculpt and Paint alpha strokes as pure operations: the brushes, Hammer's
// storage of a painted vertex, deterministic noise, revert, and seams that stay closed.
#include "geometry/displacement_sculpt.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <random>

using namespace apex;
using namespace apex::geometry;

namespace {

// A square base face at (x, y), facing +z (Face order is counter-clockwise from above).
std::array<DVec3, 4> floorQuad(double x, double y, double size, double z = 0.0) {
    return {DVec3{x, y, z}, DVec3{x + size, y, z}, DVec3{x + size, y + size, z}, DVec3{x, y + size, z}};
}

DisplacementPatch floorPatch(double x, double y, double size, int power) {
    const auto corners = floorQuad(x, y, size);
    return {corners, createDisplacement(corners, power, 0.0).value()};
}

DisplacementStroke strokeOf(std::vector<DisplacementPatch> patches) {
    auto stroke = DisplacementStroke::create(std::move(patches));
    REQUIRE(stroke);
    return std::move(*stroke);
}

// The vertex of patch p whose flat position is (x, y).
std::size_t vertexAt(const DisplacementStroke& stroke, std::size_t p, double x, double y) {
    const auto& positions = stroke.positions(p);
    for (std::size_t v = 0; v < positions.size(); ++v)
        if (std::abs(positions[v].x - x) < 1e-6 && std::abs(positions[v].y - y) < 1e-6) return v;
    FAIL("no vertex at (" << x << ", " << y << ")");
    return 0;
}

std::vector<DisplacementPatch> patchesOf(const DisplacementStroke& stroke) {
    std::vector<DisplacementPatch> patches;
    for (std::size_t p = 0; p < stroke.size(); ++p) patches.push_back(stroke.patch(p));
    return patches;
}

double worstSeam(const DisplacementStroke& stroke) {
    const auto patches = patchesOf(stroke);
    const auto seams = displacementSeams(patches);
    REQUIRE(seams);
    REQUIRE_FALSE(seams->empty());
    double worst = 0.0;
    for (const auto& seam : *seams) worst = std::max(worst, seam.gap);
    return worst;
}

// The stroke's live positions agree with D2's mesher on what it would write.
void checkLive(const DisplacementStroke& stroke) {
    for (std::size_t p = 0; p < stroke.size(); ++p) {
        const auto mesh = displacementMesh(stroke.patch(p).corners, stroke.patch(p).disp);
        REQUIRE(mesh);
        REQUIRE(mesh->positions.size() == stroke.positions(p).size());
        for (std::size_t v = 0; v < mesh->positions.size(); ++v)
            CHECK(glm::distance(mesh->positions[v], stroke.positions(p)[v]) < 1e-9);
    }
}

SculptSettings brush(SculptBrush kind, double radius, BrushFalloff falloff = BrushFalloff::Constant) {
    SculptSettings settings;
    settings.brush = kind;
    settings.radius = radius;
    settings.falloff = falloff;
    return settings;
}

} // namespace

TEST_CASE("brush falloff: 1 at the centre, 0 at and past the radius") {
    for (const auto falloff : {BrushFalloff::Constant, BrushFalloff::Linear, BrushFalloff::Smooth}) {
        CHECK(brushWeight(falloff, 0.0, 10.0) == 1.0);
        CHECK(brushWeight(falloff, 10.0, 10.0) == 0.0);
        CHECK(brushWeight(falloff, 12.0, 10.0) == 0.0);
        CHECK(brushWeight(falloff, 1.0, 0.0) == 0.0);
    }
    CHECK(brushWeight(BrushFalloff::Constant, 9.9, 10.0) == 1.0);
    CHECK(brushWeight(BrushFalloff::Linear, 2.5, 10.0) == doctest::Approx(0.75));
    CHECK(brushWeight(BrushFalloff::Smooth, 5.0, 10.0) == doctest::Approx(0.5625));
}

TEST_CASE("a sculpted vertex is stored as Hammer stores it: unit normal, non-negative distance") {
    auto disp = createDisplacement(floorQuad(0, 0, 64), 2, 0.0).value();
    disp.offsets[3] = {0, 0, 1.5};
    setDisplacementVector(disp, 3, {0, 0, -4});
    CHECK(disp.normals[3] == DVec3{0, 0, -1});
    CHECK(disp.distances[3] == 4.0);
    CHECK(disp.offsets[3] == DVec3{0, 0, 1.5});
    setDisplacementVector(disp, 3, {3, 0, 4});
    CHECK(glm::distance(disp.normals[3], DVec3{0.6, 0, 0.8}) < 1e-12);
    CHECK(disp.distances[3] == doctest::Approx(5.0));
    setDisplacementVector(disp, 3, DVec3{0.0});
    CHECK(disp.normals[3] == DVec3{0.0});
    CHECK(disp.distances[3] == 0.0);
}

TEST_CASE("Raise/Lower moves the vertices in the radius along the chosen direction") {
    auto stroke = strokeOf({floorPatch(0, 0, 128, 2)});
    CHECK(stroke.touched().empty());
    stroke.begin();
    auto settings = brush(SculptBrush::RaiseLower, 40);
    settings.amount = 3;
    stroke.sculpt(settings, {64, 64, 0});
    CHECK(stroke.dabs() == 1);
    CHECK(stroke.touched() == std::vector<std::size_t>{0});
    // The centre and its four grid neighbours (32 away) are inside; the diagonals (45) are not.
    int raised = 0;
    for (const auto& p : stroke.positions(0)) {
        raised += p.z == doctest::Approx(3.0);
        CHECK((p.z == 0.0 || p.z == doctest::Approx(3.0)));
    }
    CHECK(raised == 5);
    const auto& disp = stroke.patch(0).disp;
    const auto centre = vertexAt(stroke, 0, 64, 64);
    CHECK(disp.normals[centre] == DVec3{0, 0, 1});
    CHECK(disp.distances[centre] == doctest::Approx(3.0));
    checkLive(stroke);
    // A negative amount lowers; an axis direction moves along the axis instead.
    settings.amount = -5;
    stroke.sculpt(settings, {64, 64, 3});
    CHECK(stroke.positions(0)[centre].z == doctest::Approx(-2.0));
    CHECK(stroke.patch(0).disp.normals[centre] == DVec3{0, 0, -1});
    settings.amount = 4;
    settings.direction = SculptDirection::AxisX;
    stroke.sculpt(settings, {64, 64, -2});
    CHECK(stroke.positions(0)[centre].x == doctest::Approx(68.0));
    CHECK(stroke.positions(0)[centre].z == doctest::Approx(-2.0));
    checkLive(stroke);
}

TEST_CASE("Set height, Flatten and Smooth move toward their targets by strength") {
    SUBCASE("Set height") {
        auto stroke = strokeOf({floorPatch(0, 0, 128, 2)});
        stroke.begin();
        auto settings = brush(SculptBrush::SetHeight, 40);
        settings.height = 10;
        settings.strength = 0.5;
        const auto centre = vertexAt(stroke, 0, 64, 64);
        stroke.sculpt(settings, {64, 64, 0});
        CHECK(stroke.positions(0)[centre].z == doctest::Approx(5.0));
        stroke.sculpt(settings, {64, 64, 0});
        CHECK(stroke.positions(0)[centre].z == doctest::Approx(7.5));
        settings.strength = 1;
        settings.direction = SculptDirection::VertexNormal; // Reads as the face normal.
        stroke.sculpt(settings, {64, 64, 0});
        CHECK(stroke.positions(0)[centre].z == doctest::Approx(10.0));
        CHECK(stroke.positions(0)[vertexAt(stroke, 0, 32, 64)].z == doctest::Approx(10.0));
    }
    SUBCASE("Flatten pulls onto the plane set by the stroke's first dab") {
        auto stroke = strokeOf({floorPatch(0, 0, 128, 2)});
        stroke.begin();
        auto spike = brush(SculptBrush::RaiseLower, 10);
        spike.amount = 8;
        stroke.sculpt(spike, {64, 64, 0});
        const auto centre = vertexAt(stroke, 0, 64, 64);
        REQUIRE(stroke.positions(0)[centre].z == doctest::Approx(8.0));
        stroke.begin();
        auto flatten = brush(SculptBrush::Flatten, 40);
        flatten.direction = SculptDirection::AxisZ;
        flatten.strength = 0.5;
        stroke.sculpt(flatten, {64, 64, 2});
        CHECK(stroke.positions(0)[centre].z == doctest::Approx(5.0));
        // Later dabs keep the first dab's plane.
        stroke.sculpt(flatten, {64, 64, 5});
        CHECK(stroke.positions(0)[centre].z == doctest::Approx(3.5));
        CHECK(stroke.positions(0)[vertexAt(stroke, 0, 32, 64)].z == doctest::Approx(1.5));
    }
    SUBCASE("Smooth pulls toward the neighbours' average") {
        auto stroke = strokeOf({floorPatch(0, 0, 128, 2)});
        stroke.begin();
        auto spike = brush(SculptBrush::RaiseLower, 10);
        spike.amount = 8;
        stroke.sculpt(spike, {64, 64, 0});
        auto smooth = brush(SculptBrush::Smooth, 10);
        smooth.strength = 0.5;
        const auto centre = vertexAt(stroke, 0, 64, 64);
        stroke.sculpt(smooth, {64, 64, 8});
        CHECK(stroke.positions(0)[centre].z == doctest::Approx(4.0));
        smooth.strength = 1;
        stroke.sculpt(smooth, {64, 64, 4});
        CHECK(stroke.positions(0)[centre].z == doctest::Approx(0.0));
        checkLive(stroke);
    }
}

TEST_CASE("Noise is deterministic per seed, stroke start and dab") {
    const auto run = [](std::uint32_t seed) {
        auto stroke = strokeOf({floorPatch(0, 0, 128, 3)});
        stroke.begin();
        auto settings = brush(SculptBrush::Noise, 400);
        settings.amount = 4;
        settings.seed = seed;
        stroke.sculpt(settings, {64, 64, 0});
        return stroke.positions(0);
    };
    const auto a = run(11);
    CHECK(run(11) == a);
    CHECK(run(12) != a);
    int up = 0, down = 0;
    for (const auto& p : a) {
        CHECK(std::abs(p.z) < 4.0);
        up += p.z > 0.0;
        down += p.z < 0.0;
    }
    CHECK(up > 10);
    CHECK(down > 10);
}

TEST_CASE("paint moves alpha toward the target, from out-of-range values clamped") {
    auto patch = floorPatch(0, 0, 128, 2);
    std::ranges::fill(patch.disp.alphas, -50.0);
    auto stroke = strokeOf({patch});
    stroke.begin();
    PaintSettings settings;
    settings.falloff = BrushFalloff::Constant;
    settings.radius = 40;
    settings.strength = 0.5;
    settings.alpha = 200;
    stroke.paint(settings, {64, 64, 0});
    const auto& alphas = stroke.patch(0).disp.alphas;
    CHECK(alphas[vertexAt(stroke, 0, 64, 64)] == 100.0);
    CHECK(alphas[vertexAt(stroke, 0, 32, 64)] == 100.0);
    CHECK(alphas[vertexAt(stroke, 0, 32, 32)] == -50.0);
    CHECK(std::ranges::count(alphas, -50.0) == 20);
    settings.alpha = 900;
    settings.strength = 1;
    stroke.paint(settings, {64, 64, 0});
    CHECK(stroke.patch(0).disp.alphas[vertexAt(stroke, 0, 64, 64)] == 255.0);
    // Paint does not move the surface, so finish has nothing to retag.
    CHECK(stroke.patch(0).disp.distances == patch.disp.distances);
    REQUIRE(stroke.finish());
    CHECK(stroke.patch(0).disp.triangleTags == patch.disp.triangleTags);
}

TEST_CASE("revert puts a stroke's patches back, and reset takes a committed value") {
    const auto patch = floorPatch(0, 0, 128, 3);
    auto stroke = strokeOf({patch});
    const auto before = stroke.positions(0);
    stroke.begin();
    auto settings = brush(SculptBrush::RaiseLower, 50, BrushFalloff::Smooth);
    settings.amount = 60;
    stroke.sculpt(settings, {64, 64, 0});
    stroke.sculpt(settings, {80, 64, 0});
    REQUIRE(stroke.positions(0) != before);
    stroke.revert();
    CHECK(stroke.touched().empty());
    CHECK(stroke.positions(0) == before);
    CHECK(stroke.patch(0).disp == patch.disp);

    stroke.begin();
    stroke.sculpt(settings, {64, 64, 0});
    REQUIRE(stroke.finish());
    auto committed = stroke.patch(0).disp;
    // The slope rule of Create and Sew: the steep spike lost walkable triangles.
    auto retagged = committed;
    REQUIRE(retagDisplacement(patch.corners, retagged));
    CHECK(retagged.triangleTags == committed.triangleTags);
    CHECK(std::ranges::any_of(committed.triangleTags, [](int t) { return (t & Displacement::kTagWalkable) == 0; }));
    REQUIRE(stroke.reset(0, committed));
    stroke.begin();
    CHECK(stroke.touched().empty());
    stroke.revert();
    CHECK(stroke.patch(0).disp == committed);
    CHECK_FALSE(stroke.reset(0, createDisplacement(patch.corners, 2, 0.0).value()));
}

TEST_CASE("strokes across sewn neighbours, equal powers and T-edges, keep every seam closed") {
    std::mt19937 rng(41);
    std::uniform_real_distribution<double> along(0.0, 256.0), across(100.0, 156.0), amount(-12.0, 12.0);
    for (const auto& powers : {std::array<int, 4>{3, 3, 3, 3}, std::array<int, 4>{2, 3, 4, 3}}) {
        CAPTURE(powers[1]);
        // A 2x2 block: the corner vertex at (128, 128) is shared by four patches.
        auto stroke = strokeOf({floorPatch(0, 0, 128, powers[0]), floorPatch(128, 0, 128, powers[1]),
                                floorPatch(0, 128, 128, powers[2]), floorPatch(128, 128, 128, powers[3])});
        REQUIRE(worstSeam(stroke) < 1e-9);
        std::array<std::size_t, 4> corner{};
        for (std::size_t p = 0; p < 4; ++p) corner[p] = vertexAt(stroke, p, 128, 128);
        stroke.begin();
        const SculptBrush kinds[] = {SculptBrush::RaiseLower, SculptBrush::Noise, SculptBrush::Smooth,
                                     SculptBrush::Flatten, SculptBrush::SetHeight};
        for (int dab = 0; dab < 60; ++dab) {
            auto settings = brush(kinds[dab % 5], 48, BrushFalloff::Smooth);
            settings.amount = amount(rng);
            settings.strength = 0.6;
            settings.height = amount(rng);
            settings.direction = dab % 3 == 0 ? SculptDirection::VertexNormal : SculptDirection::FaceNormal;
            const DVec3 centre = dab % 2 ? DVec3{across(rng), along(rng), 0} : DVec3{along(rng), across(rng), 0};
            stroke.sculpt(settings, centre);
        }
        CHECK(stroke.touched().size() == 4);
        CHECK(worstSeam(stroke) < 1e-9);
        checkLive(stroke);
        // The four-way corner moved as one.
        const DVec3 moved = stroke.positions(0)[corner[0]];
        CHECK(moved.z != 0.0);
        for (std::size_t p = 1; p < 4; ++p) CHECK(glm::distance(stroke.positions(p)[corner[p]], moved) < 1e-9);
    }
}

TEST_CASE("with one patch editable, a neighbour moves only along the shared edge") {
    auto stroke = strokeOf({floorPatch(0, 0, 128, 3), floorPatch(128, 0, 128, 2)});
    stroke.setEditable({true, false});
    stroke.begin();
    auto settings = brush(SculptBrush::RaiseLower, 60);
    settings.amount = 5;
    stroke.sculpt(settings, {128, 64, 0});
    CHECK(worstSeam(stroke) < 1e-9);
    for (const auto& p : stroke.positions(1)) {
        CAPTURE(p);
        if (p.x == 128.0 && std::abs(p.y - 64.0) < 60.0) CHECK(p.z == doctest::Approx(5.0));
        else CHECK(p.z == 0.0);
    }
    // A brush entirely over the locked patch does nothing.
    stroke.begin();
    stroke.sculpt(settings, {220, 64, 0});
    CHECK(stroke.touched().empty());
}

TEST_CASE("raycast finds the live surface from either side and misses beside it") {
    auto stroke = strokeOf({floorPatch(0, 0, 128, 2), floorPatch(128, 0, 128, 2)});
    stroke.begin();
    auto settings = brush(SculptBrush::RaiseLower, 10);
    settings.amount = 20;
    stroke.sculpt(settings, {160, 64, 0});
    // Off the vertex and the triangle edges, so the hit is inside one triangle.
    const auto down = stroke.raycast({{161, 65.5, 500}, {0, 0, -1}});
    REQUIRE(down);
    CHECK(down->patch == 1);
    CHECK(down->point.x == doctest::Approx(161.0));
    CHECK(down->point.y == doctest::Approx(65.5));
    CHECK(down->point.z > 18.0);
    CHECK(down->point.z < 20.0);
    CHECK(down->t == doctest::Approx(500.0 - down->point.z));
    const auto up = stroke.raycast({{40, 45, -100}, {0, 0, 1}});
    REQUIRE(up);
    CHECK(up->patch == 0);
    CHECK(up->point.z == doctest::Approx(0.0));
    CHECK(std::abs(up->normal.z) == doctest::Approx(1.0));
    CHECK_FALSE(stroke.raycast({{300, 64, 500}, {0, 0, -1}}));
    CHECK_FALSE(stroke.raycast({{40, 45, -100}, {0, 0, -1}}));
    // An orthographic side view: a horizontal ray hits the raised spike.
    const auto side = stroke.raycast({{-1000, 66, 10}, {1, 0, 0}});
    REQUIRE(side);
    CHECK(side->patch == 1);
    CHECK(side->point.x > 128.0);
    CHECK(side->point.x < 160.0);
}
