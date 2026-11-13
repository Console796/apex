// Plan 0.8 D10: overlay projection, clipping and texture coordinates.
#include "geometry/overlay.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <random>

using namespace apex;
using namespace apex::geometry;

namespace {

kv1::Node entity(std::string_view text) {
    auto parsed = kv1::parse(std::string(text));
    REQUIRE(parsed);
    REQUIRE(!parsed->root().children().empty());
    return parsed->root().children()[0];
}

// A floor overlay at z = 0 looking down -Z (the normal points out of the floor, +Z).
OverlayQuad floorQuad(double half = 16.0) {
    OverlayQuad quad;
    quad.origin = {0, 0, 0};
    quad.basisU = {1, 0, 0};
    quad.basisV = {0, 1, 0};
    quad.normal = {0, 0, 1};
    quad.points = {DVec2{-half, -half}, DVec2{-half, half}, DVec2{half, half}, DVec2{half, -half}};
    return quad;
}

std::vector<DVec3> square(double x0, double y0, double x1, double y1, double z = 0.0) {
    return {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
}

void near(DVec2 a, DVec2 b, double eps = 1e-9) {
    CHECK(a.x == doctest::Approx(b.x).epsilon(eps));
    CHECK(a.y == doctest::Approx(b.y).epsilon(eps));
}

} // namespace

TEST_CASE("[overlay] parse reads Hammer's keys and derives a missing BasisV") {
    const auto e = entity(R"(entity {
        "classname" "info_overlay"
        "BasisOrigin" "10 20 30"
        "BasisU" "0 2 0"
        "BasisNormal" "0 0 1"
        "uv0" "-8 -4 0" "uv1" "-8 4 0" "uv2" "8 4 0" "uv3" "8 -4 0"
        "StartU" "0" "EndU" "1" "StartV" "1" "EndV" "0"
        "sides" "12 13 x 14"
    })");
    const auto quad = parseOverlay(e);
    REQUIRE(quad);
    CHECK(quad->origin == DVec3{10, 20, 30});
    CHECK(quad->basisU == DVec3{0, 1, 0});
    CHECK(quad->basisV == DVec3{-1, 0, 0}); // N x U
    CHECK(quad->startV == 1.0);
    CHECK(quad->corner(2) == DVec3{10 - 4, 20 + 8, 30});
    CHECK(parseSideList(e.find("sides")->decodedValue().value()) == std::vector<int>{12, 13, 14});
    CHECK(sideListText(std::vector<int>{1, 22, 3}) == "1 22 3");

    // overlayKeys round-trips.
    kv1::Node written = kv1::Node::block("entity");
    for (const auto& [key, value] : overlayKeys(*quad)) REQUIRE(written.append(kv1::Node::value(key, value)));
    const auto again = parseOverlay(written);
    REQUIRE(again);
    CHECK(*again == *quad);

    const auto bad = entity(R"(entity { "BasisOrigin" "0 0 0" "BasisU" "1 0 0" "BasisNormal" "1 0 0"
        "uv0" "0 0 0" "uv1" "0 1 0" "uv2" "1 1 0" "uv3" "1 0 0" })");
    CHECK_FALSE(parseOverlay(bad)); // U parallel to N.
}

TEST_CASE("[overlay] texture coordinates follow the engine's corner order") {
    auto quad = floorQuad();
    quad.startU = 0.25;
    quad.endU = 0.75;
    quad.startV = 1.0;
    quad.endV = 0.0;
    const auto corners = quad.cornerTexCoords();
    for (std::size_t i = 0; i < 4; ++i) near(quad.texCoord(quad.points[i]), corners[i]);
    near(quad.texCoord({0, 0}), {0.5, 0.5});

    // A skewed quad: the inverse bilinear map still hits every corner and edge midpoint.
    quad.points = {DVec2{-10, -12}, DVec2{-14, 9}, DVec2{18, 16}, DVec2{11, -7}};
    for (std::size_t i = 0; i < 4; ++i) {
        near(quad.texCoord(quad.points[i]), corners[i], 1e-7);
        const auto mid = (quad.points[i] + quad.points[(i + 1) % 4]) * 0.5;
        near(quad.texCoord(mid), (corners[i] + corners[(i + 1) % 4]) * 0.5, 1e-7);
    }
}

TEST_CASE("[overlay] clipping onto a flat face keeps the overlap and lifts it onto the plane") {
    const auto quad = floorQuad();
    // Fully inside a big face.
    const auto big = square(-64, -64, 64, 64);
    auto inside = clipOverlay(quad, big, {0, 0, 1}, 7);
    REQUIRE(inside);
    CHECK(inside->side == 7);
    CHECK(fragmentArea(*inside) == doctest::Approx(32.0 * 32.0));
    CHECK(fragmentArea(*inside) == doctest::Approx(quadArea(quad)));

    // Half over the face's edge.
    const auto half = square(0, -64, 64, 64);
    auto partial = clipOverlay(quad, half, {0, 0, 1});
    REQUIRE(partial);
    CHECK(fragmentArea(*partial) == doctest::Approx(16.0 * 32.0));
    for (const auto& v : partial->vertices) {
        CHECK(v.position.x >= -1e-9);
        CHECK(v.uv.x >= 0.5 - 1e-9);
    }

    // A face lower down receives the projection on its own plane.
    const auto lower = square(-64, -64, 64, 64, -40);
    auto lifted = clipOverlay(quad, lower, {0, 0, 1});
    REQUIRE(lifted);
    for (const auto& v : lifted->vertices) CHECK(v.position.z == doctest::Approx(-40));

    // Disjoint, facing away and edge-on faces get nothing.
    CHECK_FALSE(clipOverlay(quad, square(100, 100, 200, 200), {0, 0, 1}));
    CHECK_FALSE(clipOverlay(quad, std::vector<DVec3>(big.rbegin(), big.rend()), {0, 0, -1}));
    const std::vector<DVec3> wall{{0, -64, -64}, {0, 64, -64}, {0, 64, 64}, {0, -64, 64}};
    CHECK_FALSE(clipOverlay(quad, wall, {1, 0, 0}));
}

TEST_CASE("[overlay] an oblique face receives the quad's projection along the basis normal") {
    const auto quad = floorQuad();
    // A ramp rising 1 in 2 along +X: z = x / 2. Outward normal (-1, 0, 2)/sqrt5.
    const DVec3 n = glm::normalize(DVec3{-1, 0, 2});
    const std::vector<DVec3> ramp{{-64, -64, -32}, {64, -64, 32}, {64, 64, 32}, {-64, 64, -32}};
    const auto fragment = clipOverlay(quad, ramp, n);
    REQUIRE(fragment);
    // The projected area grows by 1/cos.
    CHECK(fragmentArea(*fragment) == doctest::Approx(32.0 * 32.0 * std::sqrt(5.0) / 2.0));
    for (const auto& v : fragment->vertices) CHECK(v.position.z == doctest::Approx(v.position.x / 2.0));
}

TEST_CASE("[overlay] a non-convex quad is split at its reflex corner") {
    auto quad = floorQuad();
    quad.points = {DVec2{-16, -16}, DVec2{-16, 16}, DVec2{-2, 0}, DVec2{16, -16}}; // Dart: corner 2 reflex.
    REQUIRE_FALSE(quad.problem());
    const auto fragment = clipOverlay(quad, square(-64, -64, 64, 64), {0, 0, 1});
    REQUIRE(fragment);
    // Shoelace area of the dart.
    double twice = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto a = quad.points[i], b = quad.points[(i + 1) % 4];
        twice += a.x * b.y - a.y * b.x;
    }
    CHECK(fragmentArea(*fragment) == doctest::Approx(std::abs(twice) / 2.0));
}

TEST_CASE("[overlay] clipping against a displacement follows the displaced mesh") {
    // A 128-unit power-2 displacement raised by a ridge along x.
    Displacement disp;
    disp.power = 2;
    disp.startPosition = {-64, -64, 0};
    const auto n = disp.vertexCount();
    for (std::size_t i = 0; i < n; ++i) {
        const auto column = static_cast<int>(i % 5);
        disp.normals.push_back({0, 0, 1});
        disp.distances.push_back(column == 2 ? 16.0 : 0.0);
        disp.offsets.push_back({0, 0, 0});
        disp.offsetNormals.push_back({0, 0, 1});
        disp.alphas.push_back(0.0);
    }
    for (std::size_t t = 0; t < disp.triangleCount(); ++t) disp.triangleTags.push_back(0);
    const std::array<DVec3, 4> corners{DVec3{-64, -64, 0}, DVec3{64, -64, 0}, DVec3{64, 64, 0}, DVec3{-64, 64, 0}};
    const auto mesh = displacementMesh(corners, disp);
    REQUIRE(mesh);

    const auto quad = floorQuad(40);
    const auto fragment = clipOverlay(quad, *mesh, 3);
    REQUIRE(fragment);
    CHECK(fragment->side == 3);
    // Every fragment vertex lies on the mesh: its height is the ridge's height at that (x, y).
    double highest = 0.0;
    for (const auto& v : fragment->vertices) {
        CHECK(v.position.z >= -1e-9);
        CHECK(v.position.z <= 16.0 + 1e-9);
        highest = std::max(highest, v.position.z);
        CHECK(std::abs(v.position.x) <= 40.0 + 1e-9);
        CHECK(std::abs(v.position.y) <= 40.0 + 1e-9);
    }
    CHECK(highest == doctest::Approx(16.0));
    // The draped area exceeds the flat area (the quad climbs the ridge).
    CHECK(fragmentArea(*fragment) > quadArea(quad) + 1.0);
    // Projected back onto the floor the pieces tile the quad exactly.
    double flat = 0.0;
    for (const auto& t : fragment->triangles) {
        const DVec3 a = fragment->vertices[t[0]].position, b = fragment->vertices[t[1]].position,
                    c = fragment->vertices[t[2]].position;
        flat += 0.5 * std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
    }
    CHECK(flat == doctest::Approx(80.0 * 80.0));
}

TEST_CASE("[overlay] transform carries the quad's world corners and texture range") {
    std::mt19937 rng(86);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    auto quad = floorQuad();
    quad.origin = {5, 6, 7};
    quad.points = {DVec2{-10, -12}, DVec2{-14, 9}, DVec2{18, 16}, DVec2{11, -7}};
    for (int trial = 0; trial < 50; ++trial) {
        DMat3 linear;
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) linear[c][r] = unit(rng) + (c == r ? 1.5 : 0.0);
        if (trial % 5 == 0) linear[0] = -linear[0]; // Mirrors too.
        const DVec3 translation{unit(rng) * 100, unit(rng) * 100, unit(rng) * 100};
        // Scales and rotations keep U and V orthogonal enough for the quad to be exact; the
        // corners are exact for any linear map since a corner is affine in the basis.
        const auto moved = transformOverlay(quad, linear, translation);
        for (std::size_t i = 0; i < 4; ++i) {
            const DVec3 expected = linear * quad.corner(i) + translation;
            CHECK(glm::length(moved.corner(i) - expected) < 1e-9);
        }
        CHECK(moved.startU == quad.startU);
        CHECK(moved.endV == quad.endV);
        // The normal stays perpendicular to the transformed face plane.
        CHECK(std::abs(glm::dot(moved.normal, glm::cross(linear * quad.basisU, linear * quad.basisV))) ==
              doctest::Approx(glm::length(glm::cross(linear * quad.basisU, linear * quad.basisV))).epsilon(1e-9));
    }
}

TEST_CASE("[overlay] a face-aligned quad follows the face's texture axis") {
    const auto quad = faceAlignedQuad({0, 0, 64}, {1, 0, 0}, {0, 1, 0}, {64, 32});
    CHECK(quad.basisU == DVec3{0, 1, 0});
    CHECK(quad.basisV == DVec3{0, 0, 1}); // N x U: up on a wall.
    CHECK(quadArea(quad) == doctest::Approx(64.0 * 32.0));
    // Top-left texel is at the top of the wall.
    near(quad.texCoord({-32, 16}), {0, 0});
    // A texture axis along the normal still yields a valid basis.
    const auto fallback = faceAlignedQuad({0, 0, 0}, {0, 0, 1}, {0, 0, 1}, {16, 16});
    CHECK_FALSE(fallback.problem());
}

TEST_CASE("[overlay] a transition strip runs along the shore on the water plane") {
    // Water at z = 0; a sloping beach crossing it along the Y axis at x = 0, land towards -X.
    const Plane water{{0, 0, 1}, 0.0};
    const std::vector<std::vector<DVec3>> land{{{-64, -64, 32}, {64, -64, -32}, {64, 64, -32}, {-64, 64, 32}}};
    TransitionShape shape;
    shape.landWidth = 10;
    shape.waterWidth = 20;
    const auto quads = transitionQuads(shape, water, land);
    REQUIRE(quads.size() == 1);
    const auto& q = quads[0];
    CHECK(std::abs(q.basisU.y) == doctest::Approx(1.0));
    // V points over the water (+X), land width behind.
    CHECK(q.basisV.x == doctest::Approx(1.0));
    CHECK(quadArea(q) == doctest::Approx(128.0 * 30.0));
    const auto fragment = clipOverlay(q, square(-64, -64, 64, 64), {0, 0, 1});
    REQUIRE(fragment);
    CHECK(fragmentArea(*fragment) == doctest::Approx(128.0 * 30.0));
    for (const auto& v : fragment->vertices) {
        CHECK(v.position.x >= -10 - 1e-9);
        CHECK(v.position.x <= 20 + 1e-9);
    }
}
