#include "fixtures.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace geometry_test;

TEST_CASE("box clipping builds a closed outward convex box with side attributes") {
    const auto sides = boxSides();
    const auto box = build(sides);
    REQUIRE(box.isValid());
    CHECK(box.validate().empty());
    CHECK(box.vertices.size() == 8);
    REQUIRE(box.faces.size() == 6);
    checkVector(box.bounds().min, DVec3{-64.0});
    checkVector(box.bounds().max, DVec3{64.0});
    checkVector(box.center(), DVec3{0.0});
    for (std::size_t i = 0; i < box.faces.size(); ++i) {
        const auto& face = box.faces[i];
        REQUIRE(face.vertices.size() == 4);
        CHECK(box.faceArea(face) == doctest::Approx(128.0 * 128.0));
        sameAttributes(face, sides[i]);
        CHECK(face.plane.signedDistance(box.center()) < 0.0);
    }
    CHECK(ConvexPolyhedron{}.bounds().empty());
    checkVector(ConvexPolyhedron{}.center(), DVec3{0.0});
}

TEST_CASE("wedge and sixteen sided prism survive clipping") {
    const auto wedge = build(wedgeSides());
    CHECK(wedge.isValid());
    CHECK(wedge.vertices.size() == 6);
    CHECK(wedge.faces.size() == 5);
    const auto prism = build(prismSides());
    CHECK(prism.isValid());
    CHECK(prism.vertices.size() == 32);
    CHECK(prism.faces.size() == 18);
}

TEST_CASE("off grid extremes slivers and nearly coplanar sides retain double geometry") {
    SUBCASE("off grid rotated brush") {
        const auto polyhedron = build(offGridSides());
        CHECK(polyhedron.vertices.size() == 8);
        CHECK(polyhedron.isValid());
    }
    SUBCASE("full Source range") {
        const auto polyhedron = build(boxSides(DVec3{-65536.0}, DVec3{65536.0}));
        checkVector(polyhedron.bounds().min, DVec3{-65536.0});
        checkVector(polyhedron.bounds().max, DVec3{65536.0});
    }
    SUBCASE("small brushes at both coordinate extremes") {
        for (const double origin : {-65536.0, 65535.0}) {
            const auto polyhedron = build(boxSides(DVec3{origin}, DVec3{origin + 1.0}));
            CHECK(polyhedron.vertices.size() == 8);
            checkVector(polyhedron.bounds().min, DVec3{origin});
            checkVector(polyhedron.bounds().max, DVec3{origin + 1.0});
        }
    }
    SUBCASE("thin valid sliver") {
        const auto polyhedron = build(boxSides({-64.0, -64.0, 0.0}, {64.0, 64.0, 0.0001}));
        CHECK(polyhedron.vertices.size() == 8);
        CHECK(polyhedron.bounds().max.z == doctest::Approx(0.0001).epsilon(kPlanarEps));
    }
    SUBCASE("nearly coplanar boundary faces") {
        auto sides = boxSides();
        sides.push_back(side(glm::normalize(DVec3{1.0, 0.00001, 0.0}), 64.0, 19));
        const auto polyhedron = build(sides);
        CHECK(polyhedron.faces.size() == 7);
        CHECK(polyhedron.vertices.size() == 10);
    }
}

TEST_CASE("redundant planes are deterministic and tangent cuts discard degenerate remnants") {
    auto sides = boxSides();
    sides.push_back(sides[0]);
    sides.back().sideId = 500;
    sides.push_back(side({1.0, 0.0, 0.0}, 128.0, 501));
    const auto duplicate = build(sides);
    CHECK(duplicate.faces.size() == 6);
    CHECK(duplicate.faces[0].sideId == 11);
    // The cut passes through existing corners and edges, creating a triangular prism.
    sides.push_back(side(glm::normalize(DVec3{1.0, 1.0, 0.0}), 0.0, 502));
    const auto cut = build(sides);
    CHECK(cut.faces.size() == 5);
    CHECK(cut.vertices.size() == 6);
    CHECK(cut.isValid());
}

TEST_CASE("Source clockwise boundary conversion and inverted inputs are not flipped") {
    const auto plane = planePointsFromSource({0.0, 0.0, 2.0}, {0.0, 4.0, 2.0},
                                             {3.0, 0.0, 2.0});
    REQUIRE(plane.has_value());
    checkVector(plane->normal, {0.0, 0.0, 1.0});
    CHECK(plane->dist == 2.0);
    CHECK_FALSE(planePointsFromSource({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
                                     {2.0, 0.0, 0.0}));
    auto sides = boxSides();
    sides[0].plane.normal *= -1.0;
    sides[0].plane.dist *= -1.0;
    CHECK_FALSE(ConvexPolyhedron::fromPlanes(sides));
    const auto box = build(boxSides());
    auto data = pointData(box);
    std::reverse(data[0].points.begin(), data[0].points.end());
    const auto reversed = ConvexPolyhedron::fromPointData(data);
    REQUIRE_FALSE(reversed.has_value());
    CHECK(hasProblem(reversed.error(), ProblemCode::InvertedWinding));
    auto invalid = box;
    std::reverse(invalid.faces[0].vertices.begin(), invalid.faces[0].vertices.end());
    CHECK(hasProblem(invalid.validate(), ProblemCode::InvertedWinding));
}

TEST_CASE("point data is authoritative within trust distance and falls back per side") {
    const auto original = build(offGridSides());
    SUBCASE("agreeing points") {
        const auto data = pointData(original);
        const auto result = ConvexPolyhedron::fromPointData(data);
        REQUIRE(result.has_value());
        sameVertices(*result, original);
        for (std::size_t i = 0; i < original.faces.size(); ++i) {
            sameAttributes(result->faces[i], original.faces[i]);
        }
    }
    SUBCASE("trusted serialization drift retains original coordinates exactly") {
        auto data = pointData(original);
        for (auto& value : data) {
            value.plane.dist += kPointDataTrustEps * 0.5;
        }
        const auto result = ConvexPolyhedron::fromPointData(data);
        REQUIRE(result.has_value());
        REQUIRE(result->vertices.size() == original.vertices.size());
        CHECK(result->vertices == original.vertices);
    }
    SUBCASE("half unit disagreement on one side falls back to the full plane intersection") {
        auto data = pointData(original);
        for (auto& point : data[2].points) {
            point += data[2].plane.normal * 0.5;
        }
        const auto result = ConvexPolyhedron::fromPointData(data);
        REQUIRE(result.has_value());
        sameVertices(*result, original);
        sameAttributes(result->faces[2], data[2]);
    }
    SUBCASE("missing and nonfinite point data fall back") {
        auto data = pointData(original);
        data[0].points.clear();
        data[1].points[0].x = std::numeric_limits<double>::quiet_NaN();
        const auto result = ConvexPolyhedron::fromPointData(data);
        REQUIRE(result.has_value());
        sameVertices(*result, original);
    }
    SUBCASE("trusted open or degenerate loops fail rather than getting repaired") {
        auto data = pointData(original);
        data[0].points.pop_back();
        CHECK_FALSE(ConvexPolyhedron::fromPointData(data));
        data[0].points.resize(2);
        CHECK_FALSE(ConvexPolyhedron::fromPointData(data));
    }
}

TEST_CASE("largest actual vertex triangles round trip every fixture through Source planes") {
    const std::vector fixtures{boxSides(), wedgeSides(), prismSides(), offGridSides(),
                               boxSides(DVec3{-65536.0}, DVec3{65536.0}),
                               boxSides({-64.0, -64.0, 0.0}, {64.0, 64.0, 0.0001})};
    for (const auto& sides : fixtures) {
        const auto original = build(sides);
        for (const auto& face : original.faces) {
            const auto points = original.sourcePlanePoints(face);
            REQUIRE(points.has_value());
            const double triangleArea = glm::length(glm::cross((*points)[1] - (*points)[0],
                                                               (*points)[2] - (*points)[0])) * 0.5;
            for (const auto point : *points) {
                CHECK(std::find(original.vertices.begin(), original.vertices.end(), point) !=
                      original.vertices.end());
            }
            for (const auto i : face.vertices) {
                for (const auto j : face.vertices) {
                    for (const auto k : face.vertices) {
                        const double area = glm::length(glm::cross(original.vertices[j] -
                                                                     original.vertices[i],
                                                                 original.vertices[k] -
                                                                     original.vertices[i])) * 0.5;
                        CHECK(area <= triangleArea + kMinFaceArea);
                    }
                }
            }
            CHECK(glm::dot(glm::cross((*points)[1] - (*points)[0],
                                     (*points)[2] - (*points)[0]), face.plane.normal) < 0.0);
        }
        const auto roundTrip = build(rederivedSides(original));
        sameVertices(roundTrip, original);
        CHECK(roundTrip.faces.size() == original.faces.size());
    }
}

TEST_CASE("triangulated convex faces have the same oriented area") {
    const std::vector fixtures{boxSides(), wedgeSides(), prismSides(), offGridSides()};
    for (const auto& sides : fixtures) {
        const auto polyhedron = build(sides);
        for (const auto& face : polyhedron.faces) {
            const auto triangles = polyhedron.triangulate(face);
            CHECK(triangles.size() == face.vertices.size() - 2);
            double area = 0.0;
            for (const auto& triangle : triangles) {
                const auto cross = glm::cross(polyhedron.vertices[triangle[1]] -
                                                 polyhedron.vertices[triangle[0]],
                                             polyhedron.vertices[triangle[2]] -
                                                 polyhedron.vertices[triangle[0]]);
                CHECK(glm::dot(cross, face.plane.normal) >= 0.0);
                area += glm::length(cross) * 0.5;
            }
            CHECK(area == doctest::Approx(polyhedron.faceArea(face)).epsilon(kPlanarEps));
        }
    }
}

TEST_CASE("validity diagnostics cover malformed geometry without unsafe indexing") {
    const auto box = build(boxSides());
    auto invalid = box;
    SUBCASE("empty") {
        CHECK_FALSE(ConvexPolyhedron{}.isValid());
    }
    SUBCASE("open") {
        invalid.faces.pop_back();
        CHECK(hasProblem(invalid.validate(), ProblemCode::OpenEdge));
        CHECK(hasProblem(invalid.validate(), ProblemCode::EulerCharacteristic));
    }
    SUBCASE("invalid index") {
        invalid.faces[0].vertices[0] = invalid.vertices.size();
        CHECK(hasProblem(invalid.validate(), ProblemCode::InvalidIndex));
        CHECK_FALSE(invalid.derivePlane(invalid.faces[0]));
        CHECK_FALSE(invalid.sourcePlanePoints(invalid.faces[0]));
    }
    SUBCASE("nonfinite") {
        invalid.vertices[0].x = std::numeric_limits<double>::infinity();
        CHECK(hasProblem(invalid.validate(), ProblemCode::InvalidVertex));
    }
    SUBCASE("nonplanar") {
        invalid.vertices[invalid.faces[0].vertices[0]].x += 0.5;
        CHECK(hasProblem(invalid.validate(), ProblemCode::NonPlanarFace));
    }
    SUBCASE("degenerate face") {
        invalid.faces[0].vertices = {0, 0, 0};
        CHECK(hasProblem(invalid.validate(), ProblemCode::DegenerateFace));
    }
    SUBCASE("short edge") {
        const auto a = invalid.faces[0].vertices[0];
        const auto b = invalid.faces[0].vertices[1];
        invalid.vertices[b] = invalid.vertices[a] +
                              (invalid.vertices[b] - invalid.vertices[a]) *
                                  (kMinEdgeLength / 256.0);
        CHECK(hasProblem(invalid.validate(), ProblemCode::ShortEdge));
    }
    SUBCASE("self crossing loop") {
        std::swap(invalid.faces[0].vertices[1], invalid.faces[0].vertices[2]);
        CHECK(hasProblem(invalid.validate(), ProblemCode::NonConvex));
    }
    SUBCASE("stale plane") {
        invalid.faces[0].plane.dist += 1.0;
        CHECK(hasProblem(invalid.validate(), ProblemCode::NonPlanarFace));
    }
}

TEST_CASE("invalid plane sets and subminimum geometry fail construction") {
    CHECK_FALSE(ConvexPolyhedron::fromPlanes({}));
    auto sides = boxSides();
    sides.pop_back();
    const auto unbounded = ConvexPolyhedron::fromPlanes(sides);
    REQUIRE_FALSE(unbounded.has_value());
    CHECK(hasProblem(unbounded.error(), ProblemCode::Unbounded));
    sides = boxSides();
    sides[0].plane.normal = DVec3{0.0};
    const auto invalid = ConvexPolyhedron::fromPlanes(sides);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(hasProblem(invalid.error(), ProblemCode::InvalidPlane));
    sides = boxSides();
    sides[0].plane.dist = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(ConvexPolyhedron::fromPlanes(sides));
    CHECK_FALSE(ConvexPolyhedron::fromPlanes(
        boxSides({0.0, 0.0, 0.0}, {64.0, 64.0, kMinEdgeLength * 0.5})));
}

TEST_CASE("a flattened closed tetrahedron fails despite manifold edges and Euler characteristic") {
    ConvexPolyhedron flat;
    flat.vertices = {{0.0, 0.0, 0.0}, {64.0, 0.0, 0.0},
                     {64.0, 64.0, 0.0}, {0.0, 64.0, 0.0}};
    const std::array<std::array<std::size_t, 3>, 4> loops{{{0, 2, 1}, {0, 1, 3},
                                                         {1, 2, 3}, {2, 0, 3}}};
    for (const auto& loop : loops) {
        Face face;
        face.vertices.assign(loop.begin(), loop.end());
        const auto plane = flat.derivePlane(face);
        REQUIRE(plane.has_value());
        face.plane = *plane;
        flat.faces.push_back(face);
    }
    const auto problems = flat.validate();
    CHECK(hasProblem(problems, ProblemCode::DegenerateVolume));
    CHECK_FALSE(hasProblem(problems, ProblemCode::OpenEdge));
    CHECK_FALSE(hasProblem(problems, ProblemCode::EulerCharacteristic));
}

TEST_CASE("Valve 220 UVs use world aligned axes and the standard dimension scale shift formula") {
    Face face;
    const std::array<DVec3, 6> normals{{{1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0},
                                       {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0},
                                       {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}}};
    for (const auto normal : normals) {
        face.plane.normal = normal;
        const auto uv = generateUv(face, 128, 256);
        const auto expectedU = std::abs(normal.x) == 1.0 ? DVec3{0.0, 1.0, 0.0}
                                                        : DVec3{1.0, 0.0, 0.0};
        const auto expectedV = std::abs(normal.z) == 1.0 ? DVec3{0.0, -1.0, 0.0}
                                                        : DVec3{0.0, 0.0, -1.0};
        checkVector(uv.uaxis, expectedU);
        checkVector(uv.vaxis, expectedV);
        CHECK(uv.scaleU == 1.0);
        CHECK(uv.scaleV == 1.0);
        CHECK(uv.shiftU == 0.0);
        CHECK(uv.shiftV == 0.0);
    }
    face.plane.normal = glm::normalize(DVec3{0.75, 0.25, 1.0});
    face.uv = generateUv(face, 64, 128);
    checkVector(face.uv.uaxis, {1.0, 0.0, 0.0});
    checkVector(face.uv.vaxis, {0.0, -1.0, 0.0});
    face.uv.uaxis = {1.0, 0.5, 0.0};
    face.uv.vaxis = {0.0, -1.0, 0.25};
    face.uv.scaleU = 0.5;
    face.uv.scaleV = -2.0;
    face.uv.shiftU = 8.0;
    face.uv.shiftV = -16.0;
    const auto uv = uvAt(face, {32.0, 16.0, 8.0}, 64, 128);
    CHECK(uv.x == doctest::Approx(1.375));
    CHECK(uv.y == doctest::Approx(-0.0703125));
    CHECK_THROWS_AS(generateUv(face, 0, 128), std::invalid_argument);
    CHECK_THROWS_AS(uvAt(face, DVec3{0.0}, 64, -1), std::invalid_argument);
    face.uv.scaleU = 0.0;
    CHECK_THROWS_AS(uvAt(face, DVec3{0.0}, 64, 128), std::invalid_argument);
}
