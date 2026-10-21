#include "fixtures.hpp"

#include "geometry/face_matching.hpp"

#include <algorithm>

using namespace geometry_test;

TEST_CASE("matching a rebuild preserves every side ID and texture field independent of order") {
    const std::vector fixtures{boxSides(), wedgeSides(), prismSides(), offGridSides()};
    for (const auto& sides : fixtures) {
        const auto original = build(sides);
        auto planes = rederivedSides(original);
        std::reverse(planes.begin(), planes.end());
        auto rebuilt = build(planes);
        const auto result = matchFaces(original, rebuilt);
        CHECK(result.unmatchedNew.empty());
        CHECK(result.vanishedOld.empty());
        REQUIRE(result.matched.size() == original.faces.size());
        for (const auto& match : result.matched) {
            sameAttributes(rebuilt.faces[match.newIndex], original.faces[match.oldIndex]);
        }
        sameVertices(rebuilt, original);
    }
}

TEST_CASE("corner clip adds one face with expected normal neighbour and loses no faces") {
    const auto original = build(boxSides());
    auto sides = rederivedSides(original);
    const auto normal = glm::normalize(DVec3{2.0, 1.0, 0.5});
    sides.push_back(side(normal, 80.0, 99));
    auto clipped = build(sides);
    REQUIRE(clipped.faces.size() == 7);
    const auto result = matchFaces(original, clipped);
    CHECK(result.matched.size() == 6);
    CHECK(result.vanishedOld.empty());
    REQUIRE(result.unmatchedNew.size() == 1);
    const auto& added = result.unmatchedNew[0];
    REQUIRE(added.inheritFrom.has_value());
    checkVector(original.faces[*added.inheritFrom].plane.normal, {1.0, 0.0, 0.0});
    checkVector(clipped.faces[added.newIndex].plane.normal, normal);
    CHECK(clipped.faces[added.newIndex].sideId == 99); // Allocation is the caller's job.
    sameAttributes(clipped.faces[added.newIndex], sides.back());
}

TEST_CASE("clip replacing an entire boundary reports the vanished old side") {
    const auto original = build(boxSides());
    auto sides = rederivedSides(original);
    sides.push_back(side({1.0, 0.0, 0.0}, 0.0, 99));
    auto clipped = build(sides);
    const auto result = matchFaces(original, clipped);
    CHECK(result.matched.size() == 5);
    REQUIRE(result.vanishedOld.size() == 1);
    REQUIRE(result.unmatchedNew.size() == 1);
    const auto vanished = result.vanishedOld[0];
    CHECK(original.faces[vanished].sideId == 11);
    CHECK(result.unmatchedNew[0].inheritFrom == vanished);
}

TEST_CASE("plane matching boundaries duplicates and inheritance ties are deterministic") {
    ConvexPolyhedron original;
    Face face;
    static_cast<FaceAttributes&>(face) = side({0.0, 0.0, 1.0}, 0.0, 1);
    face.plane = {{0.0, 0.0, 1.0}, 0.0};
    original.faces = {face, face};
    original.faces[1].sideId = 2;
    auto rebuilt = original;
    rebuilt.faces.push_back(face);
    const auto result = matchFaces(original, rebuilt);
    REQUIRE(result.matched.size() == 2);
    CHECK(result.matched[0].oldIndex == 0);
    CHECK(result.matched[1].oldIndex == 1);
    CHECK(result.vanishedOld.empty());
    REQUIRE(result.unmatchedNew.size() == 1);
    CHECK(result.unmatchedNew[0].newIndex == 2);
    CHECK(result.unmatchedNew[0].inheritFrom == 0);
    SUBCASE("distance inside tolerance") {
        rebuilt = original;
        for (auto& value : rebuilt.faces) {
            value.plane.dist += kPlanarEps * 0.5;
        }
        CHECK(matchFaces(original, rebuilt).unmatchedNew.empty());
    }
    SUBCASE("distance outside tolerance") {
        rebuilt = original;
        rebuilt.faces[0].plane.dist += kPlanarEps * 2.0;
        CHECK(matchFaces(original, rebuilt).unmatchedNew.size() == 1);
    }
    SUBCASE("normal outside tolerance") {
        rebuilt = original;
        rebuilt.faces[0].plane.normal = glm::normalize(DVec3{kNormalMatchEps * 2.0, 0.0, 1.0});
        CHECK(matchFaces(original, rebuilt).unmatchedNew.size() == 1);
    }
    SUBCASE("opposite orientation cannot match") {
        rebuilt = original;
        rebuilt.faces[0].plane.normal *= -1.0;
        CHECK(matchFaces(original, rebuilt).unmatchedNew.size() == 1);
    }
}

TEST_CASE("plane matching augments assignments near overlapping tolerance intervals") {
    ConvexPolyhedron original;
    Face face;
    original.faces = {face, face};
    original.faces[0].sideId = 1;
    original.faces[1].sideId = 2;
    original.faces[1].plane.dist = kPlanarEps * 1.5;
    auto rebuilt = original;
    rebuilt.faces[0].plane.dist = kPlanarEps * 0.6; // Both old planes, prefers old 0.
    rebuilt.faces[1].plane.dist = -kPlanarEps * 0.6; // Only old 0.
    const auto result = matchFaces(original, rebuilt);
    REQUIRE(result.matched.size() == 2);
    CHECK(result.unmatchedNew.empty());
    CHECK(result.vanishedOld.empty());
    CHECK(rebuilt.faces[0].sideId == 2);
    CHECK(rebuilt.faces[1].sideId == 1);
}

TEST_CASE("matching empty inputs reports new faces without inventing IDs or neighbours") {
    const ConvexPolyhedron empty;
    auto box = build(boxSides());
    const auto added = matchFaces(empty, box);
    CHECK(added.matched.empty());
    CHECK(added.vanishedOld.empty());
    REQUIRE(added.unmatchedNew.size() == 6);
    for (const auto& face : added.unmatchedNew) {
        CHECK_FALSE(face.inheritFrom.has_value());
    }
    ConvexPolyhedron removed;
    const auto vanished = matchFaces(box, removed);
    CHECK(vanished.vanishedOld.size() == 6);
    CHECK(vanished.unmatchedNew.empty());
}
