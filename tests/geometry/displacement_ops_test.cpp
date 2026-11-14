// Plan 0.8 D6: Create, Sew and Subdivide as pure operations, and the seam measure behind the
// "unsewn displacement edge" problem. Hammer's own output for a sewn patch (D3 fixture 4) does not
// exist yet; these are Apex-side property tests.
#include "geometry/displacement_ops.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <random>

using namespace apex;
using namespace apex::geometry;

namespace {

// A square base face of the given size at (x, y), facing +z: Face order is counter-clockwise
// from outside (above).
std::array<DVec3, 4> floorQuad(double x, double y, double size, double z = 0.0) {
    return {DVec3{x, y, z}, DVec3{x + size, y, z}, DVec3{x + size, y + size, z}, DVec3{x, y + size, z}};
}

// The flat bilinear grid displacementMesh builds from the start corner, row c0->c1, column c0->c3
// in Source's (clockwise) order.
std::vector<DVec3> bilinear(std::array<DVec3, 4> corners, DVec3 start, int size) {
    std::array<DVec3, 4> source;
    for (std::size_t k = 0; k < 4; ++k) source[k] = corners[3 - k];
    std::size_t first = 0;
    for (std::size_t k = 0; k < 4; ++k)
        if (glm::distance(source[k], start) < glm::distance(source[first], start)) first = k;
    std::array<DVec3, 4> c;
    for (std::size_t k = 0; k < 4; ++k) c[k] = source[(first + k) % 4];
    std::vector<DVec3> grid;
    for (int i = 0; i < size; ++i)
        for (int j = 0; j < size; ++j) {
            const double t = double(i) / (size - 1), s = double(j) / (size - 1);
            const auto left = c[0] + (c[1] - c[0]) * t, right = c[3] + (c[2] - c[3]) * t;
            grid.push_back(left + (right - left) * s);
        }
    return grid;
}

double worstSeam(std::span<const DisplacementPatch> patches) {
    const auto seams = displacementSeams(patches);
    REQUIRE(seams);
    double worst = 0.0;
    for (const auto& seam : *seams) worst = std::max(worst, seam.gap);
    return worst;
}

} // namespace

TEST_CASE("a created displacement is Hammer's flat grid on its base face") {
    std::mt19937 rng(83);
    std::uniform_real_distribution<double> wobble(-20.0, 20.0);
    for (int power = 2; power <= 4; ++power) {
        for (int trial = 0; trial < 4; ++trial) {
            CAPTURE(power);
            CAPTURE(trial);
            // A tilted, non-rectangular quad: (x, y) wobbled, z a plane through them.
            std::array<DVec3, 4> corners = floorQuad(100, -300, 256);
            for (auto& c : corners) {
                c.x += wobble(rng);
                c.y += wobble(rng);
                c.z = 0.25 * c.x - 0.5 * c.y + 7.0;
            }
            const auto disp = createDisplacement(corners, power, 12.5);
            REQUIRE(disp);
            CHECK_FALSE(disp->problem());
            CHECK(disp->power == power);
            CHECK(disp->elevation == 12.5);
            CHECK(disp->flags == 0);
            CHECK(disp->subdiv == 0);
            const auto normal = displacementBaseNormal(corners).value();
            CHECK(normal.z > 0.0);
            for (std::size_t i = 0; i < disp->vertexCount(); ++i) {
                CHECK(disp->normals[i] == normal);
                CHECK(disp->offsetNormals[i] == normal);
                CHECK(disp->distances[i] == 0.0);
                CHECK(disp->offsets[i] == DVec3{0.0});
                CHECK(disp->alphas[i] == 0.0);
            }
            CHECK(std::ranges::count(disp->allowedVerts, true) == 289);
            const auto mesh = displacementMesh(corners, *disp);
            REQUIRE(mesh);
            const auto flat = bilinear(corners, disp->startPosition, disp->size());
            REQUIRE(mesh->positions.size() == flat.size());
            for (std::size_t i = 0; i < flat.size(); ++i) CHECK(glm::distance(mesh->positions[i], flat[i]) < 1e-9);
        }
    }
}

TEST_CASE("a new displacement starts at the corner nearest the base's bounding-box minimum") {
    // test1_disp.vmf side 355: Strata Hammer wrote startposition [-96 -1632 64].
    const std::array<DVec3, 4> corners{DVec3{-96, -1440, 64}, DVec3{-96, -1632, 64}, DVec3{96, -1632, 64},
                                       DVec3{96, -1440, 64}};
    CHECK(displacementStartCorner(corners) == DVec3{-96, -1632, 64});
    // Each rotation of the loop gives the same corner.
    for (std::size_t r = 0; r < 4; ++r) {
        std::array<DVec3, 4> rotated;
        for (std::size_t k = 0; k < 4; ++k) rotated[k] = corners[(k + r) % 4];
        CHECK(displacementStartCorner(rotated) == DVec3{-96, -1632, 64});
    }
}

TEST_CASE("triangle tags follow the slope: walkable and buildable floors, nothing on walls") {
    const auto floor = createDisplacement(floorQuad(0, 0, 128), 2, 0).value();
    CHECK(std::ranges::all_of(floor.triangleTags, [](int t) {
        return t == (Displacement::kTagWalkable | Displacement::kTagBuildable);
    }));
    // A wall (facing +y in Face order).
    const std::array<DVec3, 4> wall{DVec3{0, 0, 0}, DVec3{0, 0, 128}, DVec3{128, 0, 128}, DVec3{128, 0, 0}};
    REQUIRE(displacementBaseNormal(wall).value().y > 0.99);
    const auto upright = createDisplacement(wall, 2, 0).value();
    CHECK(std::ranges::all_of(upright.triangleTags, [](int t) { return t == 0; }));
    // Force bits win over the slope, and unknown bits are kept.
    auto forced = upright;
    forced.triangleTags[0] = Displacement::kTagForceWalkable | Displacement::kTagForceWalkableValue | 64;
    REQUIRE(retagDisplacement(wall, forced));
    CHECK(forced.triangleTags[0] ==
          (Displacement::kTagForceWalkable | Displacement::kTagForceWalkableValue | Displacement::kTagWalkable | 64));
}

TEST_CASE("create refuses a bad power, a non-quad and a degenerate base") {
    CHECK_FALSE(createDisplacement(floorQuad(0, 0, 64), 1, 0));
    CHECK_FALSE(createDisplacement(floorQuad(0, 0, 64), 5, 0));
    const std::array<DVec3, 3> triangle{DVec3{0, 0, 0}, DVec3{64, 0, 0}, DVec3{0, 64, 0}};
    CHECK_FALSE(createDisplacement(triangle, 3, 0));
    const std::array<DVec3, 4> line{DVec3{0, 0, 0}, DVec3{64, 0, 0}, DVec3{128, 0, 0}, DVec3{32, 0, 0}};
    CHECK_FALSE(createDisplacement(line, 3, 0));
}

// TODO(0.8 D3 fixture 4): replace this Apex-built patch with the owner's Strata Hammer 3x3 sewn
// patch (mixed powers, corners and T-edges), and compare Apex's Sew with Hammer's output.
TEST_CASE("sew closes every shared edge of a 3x3 patch with mixed powers") {
    const int powers[3][3] = {{2, 3, 2}, {3, 4, 3}, {2, 3, 4}};
    std::vector<DisplacementPatch> patches;
    std::mt19937 rng(8);
    std::uniform_real_distribution<double> height(-24.0, 24.0), shift(-3.0, 3.0);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x) {
            const auto corners = floorQuad(x * 128.0, y * 128.0, 128.0, 32.0);
            auto disp = createDisplacement(corners, powers[y][x], 0).value();
            // A deliberate offset: every vertex its own height and a small sideways offset.
            for (std::size_t i = 0; i < disp.vertexCount(); ++i) {
                disp.distances[i] = height(rng);
                disp.offsets[i] = {shift(rng), shift(rng), 0.0};
            }
            patches.push_back({corners, std::move(disp)});
        }
    const auto before = patches;
    const auto seams = displacementSeams(patches);
    REQUIRE(seams);
    CHECK(seams->size() == 12); // The grid's twelve inner edges; diagonal neighbours share only a corner.
    CHECK(worstSeam(patches) > 1.0);

    const auto changed = sewDisplacements(patches);
    REQUIRE(changed);
    CHECK(*changed == 9);
    CHECK(worstSeam(patches) < 1e-6);
    // The base stays put; interior vertices do not move; the result is still a valid displacement.
    for (std::size_t p = 0; p < patches.size(); ++p) {
        CHECK_FALSE(patches[p].disp.problem());
        CHECK(patches[p].corners == before[p].corners);
        const auto after = displacementMesh(patches[p].corners, patches[p].disp).value();
        const auto old = displacementMesh(before[p].corners, before[p].disp).value();
        const int n = patches[p].disp.size();
        for (int i = 1; i + 1 < n; ++i)
            for (int j = 1; j + 1 < n; ++j) {
                const auto at = static_cast<std::size_t>(i * n + j);
                CHECK(after.positions[at] == old.positions[at]);
            }
    }
    // The middle corner of four patches is their average.
    DVec3 sum{0.0};
    const auto cornerOf = [&](const std::vector<DisplacementPatch>& set, std::size_t p) {
        const auto mesh = displacementMesh(set[p].corners, set[p].disp).value();
        const auto flat = bilinear(set[p].corners, set[p].disp.startPosition, set[p].disp.size());
        const auto it = std::ranges::min_element(flat, {}, [](DVec3 v) { return glm::distance(v, DVec3{128, 128, 32}); });
        return mesh.positions[static_cast<std::size_t>(it - flat.begin())];
    };
    for (const std::size_t p : {0u, 1u, 3u, 4u}) sum += cornerOf(before, p);
    CHECK(glm::distance(cornerOf(patches, 4), sum / 4.0) < 1e-6);
    // Sewing again changes nothing.
    CHECK(sewDisplacements(patches).value() == 0);
}

TEST_CASE("sew moves a finer edge's extra vertices onto the coarser edge's line") {
    std::vector<DisplacementPatch> patches{{floorQuad(0, 0, 128), createDisplacement(floorQuad(0, 0, 128), 2, 0).value()},
                                           {floorQuad(128, 0, 128), createDisplacement(floorQuad(128, 0, 128), 4, 0).value()}};
    // Raise the fine side's whole shared edge by 10; the coarse side stays flat.
    const auto fine = displacementMesh(patches[1].corners, patches[1].disp).value();
    for (std::size_t k = 0; k < fine.positions.size(); ++k)
        if (std::abs(fine.positions[k].x - 128.0) < 1e-9) patches[1].disp.distances[k] = 10.0;
    CHECK(worstSeam(patches) == doctest::Approx(10.0));
    REQUIRE(sewDisplacements(patches));
    CHECK(worstSeam(patches) < 1e-6);
    // Shared vertices meet halfway, and the T-vertices lie on the straight coarse edge.
    for (const auto& patch : patches) {
        const auto mesh = displacementMesh(patch.corners, patch.disp).value();
        for (const auto& p : mesh.positions)
            if (std::abs(p.x - 128.0) < 1e-9) CHECK(p.z == doctest::Approx(5.0));
    }
}

TEST_CASE("seams ignore patches that do not touch and measure sewn ones as closed") {
    std::vector<DisplacementPatch> patches{{floorQuad(0, 0, 64), createDisplacement(floorQuad(0, 0, 64), 3, 0).value()},
                                           {floorQuad(64, 0, 64), createDisplacement(floorQuad(64, 0, 64), 3, 0).value()},
                                           {floorQuad(512, 0, 64), createDisplacement(floorQuad(512, 0, 64), 3, 0).value()}};
    const auto seams = displacementSeams(patches);
    REQUIRE(seams);
    REQUIRE(seams->size() == 1);
    CHECK(seams->front().a == 0);
    CHECK(seams->front().b == 1);
    CHECK(seams->front().gap == 0.0);
}

TEST_CASE("subdivide sets subdiv, keeps a flat grid flat and its boundary in place, and smooths a bump") {
    const auto corners = floorQuad(0, 0, 256);
    auto flat = createDisplacement(corners, 3, 0).value();
    auto subdivided = flat;
    REQUIRE(subdivideDisplacement(corners, subdivided));
    CHECK(subdivided.subdiv == 1);
    const auto a = displacementMesh(corners, flat).value(), b = displacementMesh(corners, subdivided).value();
    for (std::size_t i = 0; i < a.positions.size(); ++i) CHECK(glm::distance(a.positions[i], b.positions[i]) < 1e-9);

    auto bumpy = flat;
    const int n = bumpy.size();
    const auto centre = static_cast<std::size_t>((n / 2) * n + n / 2);
    bumpy.distances[centre] = 32.0;
    for (int k = 0; k < n; ++k) bumpy.distances[static_cast<std::size_t>(k)] = 8.0; // An edge row.
    auto smooth = bumpy;
    REQUIRE(subdivideDisplacement(corners, smooth));
    const auto before = displacementMesh(corners, bumpy).value(), after = displacementMesh(corners, smooth).value();
    CHECK(after.positions[centre].z < before.positions[centre].z);
    for (int k = 0; k < n; ++k) CHECK(after.positions[static_cast<std::size_t>(k)] == before.positions[static_cast<std::size_t>(k)]);
    CHECK(smooth.distances == bumpy.distances); // The smoothing lives in the offsets.
}

TEST_CASE("only a displacement with no distances, offsets or alphas counts as flat") {
    auto disp = createDisplacement(floorQuad(0, 0, 64), 2, 0).value();
    CHECK(displacementIsFlat(disp));
    disp.alphas[3] = 1.0;
    CHECK_FALSE(displacementIsFlat(disp));
}
