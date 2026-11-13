// Plan 0.8 D2/D4 property tests: grid shape, and transform equivariance
// mesh(transform(d)) == transform(mesh(d)) over random affine maps, mirrors included.
#include "geometry/displacement.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <stdexcept>

using namespace apex;
using namespace apex::geometry;

namespace {

Displacement randomDisplacement(std::mt19937& rng, int power) {
    std::uniform_real_distribution<double> unit(-1.0, 1.0), dist(-40.0, 40.0), alpha(0.0, 255.0);
    Displacement d;
    d.power = power;
    d.flags = 4;
    d.elevation = 3.5;
    const auto n = d.vertexCount();
    for (std::size_t i = 0; i < n; ++i) {
        DVec3 normal{unit(rng), unit(rng), unit(rng)};
        // Some zero normals, as Hammer writes for untouched vertices.
        d.normals.push_back(i % 7 == 3 ? DVec3{0.0} : glm::normalize(normal));
        d.distances.push_back(dist(rng));
        d.offsets.push_back({unit(rng) * 4, unit(rng) * 4, unit(rng) * 4});
        d.offsetNormals.push_back(glm::normalize(DVec3{unit(rng), unit(rng), 2.0}));
        d.alphas.push_back(std::round(alpha(rng)));
    }
    for (std::size_t t = 0; t < d.triangleCount(); ++t) d.triangleTags.push_back(static_cast<int>(t % 16));
    d.allowedVerts.assign(320, true);
    for (std::size_t i = 0; i < n; i += 5) d.allowedVerts[i] = false;
    return d;
}

// A convex planar quad, counter-clockwise about +normal of a random frame.
std::array<DVec3, 4> randomQuad(std::mt19937& rng) {
    std::uniform_real_distribution<double> unit(-1.0, 1.0), size(32.0, 256.0), wobble(-0.2, 0.2);
    const auto a = glm::normalize(DVec3{unit(rng), unit(rng), unit(rng)});
    const auto b = glm::normalize(glm::cross(a, glm::normalize(DVec3{unit(rng), unit(rng), unit(rng) + 2.0})));
    const DVec3 origin{unit(rng) * 1000, unit(rng) * 1000, unit(rng) * 1000};
    const double w = size(rng), h = size(rng);
    // Corners of a perturbed rectangle stay convex for wobble < 0.25.
    const std::array<DVec2, 4> uv{DVec2{0 + wobble(rng), 0 + wobble(rng)}, DVec2{1 + wobble(rng), 0 + wobble(rng)},
                                  DVec2{1 + wobble(rng), 1 + wobble(rng)}, DVec2{0 + wobble(rng), 1 + wobble(rng)}};
    std::array<DVec3, 4> result;
    for (std::size_t k = 0; k < 4; ++k) result[k] = origin + a * (uv[k].x * w) + b * (uv[k].y * h);
    return result;
}

DMat3 randomLinear(std::mt19937& rng) {
    std::uniform_real_distribution<double> unit(-1.5, 1.5);
    for (;;) {
        DMat3 m;
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) m[c][r] = unit(rng);
        if (std::abs(glm::determinant(m)) > 0.2) return m;
    }
}

DMat3 mirror(DVec3 normal) {
    normal = glm::normalize(normal);
    return DMat3{1.0} - 2.0 * glm::outerProduct(normal, normal);
}

void checkEquivariant(const Displacement& d, const std::array<DVec3, 4>& corners, const DMat3& linear, DVec3 t) {
    const auto before = displacementMesh(corners, d);
    REQUIRE(before);
    // The base face as applyToSolid leaves it: mapped, and reversed when orientation flips.
    std::array<DVec3, 4> moved;
    for (std::size_t k = 0; k < 4; ++k) moved[k] = linear * corners[k] + t;
    const bool flips = glm::determinant(linear) < 0.0;
    if (flips) std::ranges::reverse(moved);
    const auto carried = transformDisplacement(d, linear, t);
    REQUIRE(carried.problem() == std::nullopt);
    const auto after = displacementMesh(moved, carried);
    REQUIRE(after);
    const auto size = static_cast<std::size_t>(d.size());
    const auto image = [&](std::size_t i) { return flips ? (i % size) * size + i / size : i; };
    for (std::size_t i = 0; i < before->positions.size(); ++i) {
        const auto expected = linear * before->positions[i] + t;
        const auto& actual = after->positions[image(i)];
        CAPTURE(i);
        REQUIRE(glm::distance(actual, expected) < 1e-7 * (1.0 + glm::length(expected)));
        CHECK(after->alphas[image(i)] == before->alphas[i]);
        if (i < d.allowedVerts.size()) CHECK(carried.allowedVerts[image(i)] == d.allowedVerts[i]);
    }
    // Winding is checked on the flat grid: a shear can tip a steep displaced triangle past the
    // base plane's normal, but the flat triangles stay in the plane, where the sign is invariant.
    auto flat = d;
    std::ranges::fill(flat.distances, 0.0);
    std::ranges::fill(flat.offsets, DVec3{0.0});
    const auto flatBefore = displacementMesh(corners, flat).value();
    const auto flatAfter = displacementMesh(moved, transformDisplacement(flat, linear, t)).value();
    std::map<std::set<std::uint32_t>, std::pair<int, DVec3>> triangles;
    for (std::size_t k = 0; k < after->triangles.size(); ++k) {
        const auto& tri = after->triangles[k];
        const auto& p = flatAfter.positions;
        triangles[{tri[0], tri[1], tri[2]}] = {after->tags[k], glm::cross(p[tri[1]] - p[tri[0]], p[tri[2]] - p[tri[0]])};
    }
    // Each triangle maps to a triangle with its tag, and still winds counter-clockwise about the
    // (moved) base face: mirrored meshes are not turned inside out.
    const auto normalBefore = glm::cross(corners[1] - corners[0], corners[2] - corners[0]);
    const auto normalAfter = glm::cross(moved[1] - moved[0], moved[2] - moved[0]);
    for (std::size_t k = 0; k < before->triangles.size(); ++k) {
        const auto& tri = before->triangles[k];
        const auto found = triangles.find({static_cast<std::uint32_t>(image(tri[0])), static_cast<std::uint32_t>(image(tri[1])),
                                           static_cast<std::uint32_t>(image(tri[2]))});
        REQUIRE(found != triangles.end());
        CHECK(found->second.first == before->tags[k]);
        const auto& p = flatBefore.positions;
        const auto crossBefore = glm::cross(p[tri[1]] - p[tri[0]], p[tri[2]] - p[tri[0]]);
        CHECK(glm::dot(crossBefore, normalBefore) > 0);
        CHECK(glm::dot(found->second.second, normalAfter) > 0);
    }
}

} // namespace

TEST_CASE("displacement mesh grid shape and triangle count follow the power") {
    std::mt19937 rng(7);
    for (int power = Displacement::kMinPower; power <= Displacement::kMaxPower; ++power) {
        const auto d = randomDisplacement(rng, power);
        const auto quad = randomQuad(rng);
        const auto mesh = displacementMesh(quad, d);
        REQUIRE(mesh);
        const auto size = static_cast<std::size_t>((1 << power) + 1);
        CHECK(mesh->size == static_cast<int>(size));
        CHECK(mesh->positions.size() == size * size);
        CHECK(mesh->triangles.size() == 2 * (size - 1) * (size - 1));
        for (const auto& tri : mesh->triangles)
            for (const auto v : tri) CHECK(v < size * size);
    }
    auto bad = randomDisplacement(rng, 2);
    bad.distances.pop_back();
    CHECK(bad.problem());
    CHECK_FALSE(displacementMesh(randomQuad(rng), bad));
    bad = randomDisplacement(rng, 2);
    bad.power = 5;
    CHECK(bad.problem());
    const std::array<DVec3, 3> triangle{DVec3{0, 0, 0}, DVec3{1, 0, 0}, DVec3{0, 1, 0}};
    CHECK_FALSE(displacementMesh(triangle, randomDisplacement(rng, 2)));
}

TEST_CASE("displacement start corner is the base corner nearest startPosition") {
    std::mt19937 rng(11);
    auto d = randomDisplacement(rng, 2);
    std::ranges::fill(d.distances, 0.0);
    std::ranges::fill(d.offsets, DVec3{0.0});
    // Counter-clockwise from +Z; Source's clockwise order is the reverse.
    const std::array<DVec3, 4> quad{DVec3{0, 0, 0}, DVec3{100, 0, 0}, DVec3{100, 50, 0}, DVec3{0, 50, 0}};
    for (std::size_t k = 0; k < 4; ++k) {
        d.startPosition = quad[k] + DVec3{1, -1, 0.5}; // Near, not on, the corner.
        const auto mesh = displacementMesh(quad, d).value();
        CAPTURE(k);
        CHECK(glm::distance(mesh.positions[0], quad[k]) < 1e-12);
        // Row 1 starts towards the next corner clockwise, column 1 towards the next counter-clockwise.
        CHECK(glm::distance(mesh.positions[5], quad[(k + 3) % 4] * 0.25 + quad[k] * 0.75) < 1e-12);
        CHECK(glm::distance(mesh.positions[1], quad[(k + 1) % 4] * 0.25 + quad[k] * 0.75) < 1e-12);
        CHECK(glm::distance(mesh.positions[24], quad[(k + 2) % 4]) < 1e-12);
    }
}

TEST_CASE("displacement corners skip collinear subdivision vertices") {
    ConvexPolyhedron poly;
    poly.vertices = {{0, 0, 0}, {50, 0, 0}, {100, 0, 0}, {100, 50, 0}, {0, 50, 0}};
    Face face;
    face.vertices = {0, 1, 2, 3, 4};
    const auto corners = displacementCorners(poly, face);
    REQUIRE(corners);
    CHECK((*corners)[1] == DVec3{100, 0, 0});
    face.vertices = {0, 2, 3};
    CHECK_FALSE(displacementCorners(poly, face));
}

TEST_CASE("transformDisplacement is equivariant under random affine maps, mirrors and scales") {
    std::mt19937 rng(20261110);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    for (int trial = 0; trial < 60; ++trial) {
        CAPTURE(trial);
        const int power = Displacement::kMinPower + trial % 3;
        auto d = randomDisplacement(rng, power);
        const auto quad = randomQuad(rng);
        d.startPosition = quad[static_cast<std::size_t>(trial) % 4];
        const DVec3 t{unit(rng) * 500, unit(rng) * 500, unit(rng) * 500};
        DMat3 linear;
        switch (trial % 5) {
        case 0: linear = randomLinear(rng); break;                                            // Shear and scale.
        case 1: linear = mirror({unit(rng), unit(rng), unit(rng)}); break;                    // Plain mirror.
        case 2: linear = DMat3{2.5, 0, 0, 0, 0.4, 0, 0, 0, 1.3}; break;                      // Non-uniform scale.
        case 3: linear = mirror({1, 0, 0}) * DMat3{1, 0.7, 0, 0, 1, 0, 0.2, 0, 1}; break;    // Mirror with shear.
        default: linear = -DMat3{1.0}; break;                                                 // Point reflection.
        }
        checkEquivariant(d, quad, linear, t);
    }
}

TEST_CASE("transformDisplacement round-trips through the inverse map") {
    std::mt19937 rng(5);
    const auto d = randomDisplacement(rng, 3);
    for (const auto& linear : {DMat3{0, 1, 0, -1, 0, 0, 0, 0, 1}, mirror({0.3, -1, 0.2}), DMat3{2, 0, 0, 0.5, 1, 0, 0, 0, 3}}) {
        const DVec3 t{12, -40, 7};
        const auto back = transformDisplacement(transformDisplacement(d, linear, t), glm::inverse(linear),
                                                -(glm::inverse(linear) * t));
        CHECK(glm::distance(back.startPosition, d.startPosition) < 1e-9);
        for (std::size_t i = 0; i < d.vertexCount(); ++i) {
            CHECK(glm::distance(back.normals[i] * back.distances[i], d.normals[i] * d.distances[i]) < 1e-9);
            CHECK(glm::distance(back.offsets[i], d.offsets[i]) < 1e-9);
            CHECK(back.alphas[i] == d.alphas[i]);
        }
        CHECK(back.triangleTags == d.triangleTags);
        CHECK(back.allowedVerts == d.allowedVerts);
    }
    CHECK_THROWS_AS(transformDisplacement(d, DMat3{0.0}, DVec3{0.0}), std::invalid_argument);
}
