#include "fixtures.hpp"

#include "geometry/solid_block.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <stdexcept>

using namespace geometry_test;

namespace {
ConvexPolyhedron hull(std::span<const DVec3> points) {
    auto result = convexHull(points);
    if (!result) for (const auto& problem : result.error()) MESSAGE(problem.message);
    REQUIRE(result.has_value());
    return std::move(*result);
}
void contains(const ConvexPolyhedron& poly, std::span<const DVec3> points) {
    for (const auto& face : poly.faces)
        for (const auto p : points) CHECK(face.plane.signedDistance(p) <= kPlanarEps);
}
double totalVolume(std::span<const ConvexPolyhedron> pieces) {
    double result = 0.0;
    for (const auto& piece : pieces) result += piece.volume();
    return result;
}
std::optional<ConvexPolyhedron> intersection(const ConvexPolyhedron& a,
                                             const ConvexPolyhedron& b) {
    std::optional<ConvexPolyhedron> result = a;
    for (const auto& face : b.faces) {
        if (!result) break;
        result = split(*result, face.plane).back;
    }
    return result;
}
}

TEST_CASE("hull merges cube triangles to six quads and defaults attributes") {
    const auto cube = build(boxSides());
    const auto poly = hull(cube.vertices);
    REQUIRE(poly.isValid());
    CHECK(poly.faces.size() == 6);
    sameVertices(poly, cube);
    CHECK(poly.volume() == doctest::Approx(128.0 * 128.0 * 128.0));
    for (const auto& face : poly.faces) {
        CHECK(face.vertices.size() == 4);
        sameAttributes(face, FaceAttributes{});
        CHECK_FALSE(face.isCap);
        CHECK_FALSE(face.carverFace);
    }
}

TEST_CASE("hull handles tetrahedra duplicates interior and welded points") {
    const std::vector<DVec3> tetra{{0, 0, 0}, {64, 0, 0}, {0, 64, 0}, {0, 0, 64}};
    const auto original = hull(tetra);
    CHECK(original.faces.size() == 4);
    CHECK(original.volume() == doctest::Approx(64.0 * 64.0 * 64.0 / 6.0));
    auto points = tetra;
    points.insert(points.end(), tetra.begin(), tetra.end());
    points.push_back({8, 8, 8});
    points.push_back({kPointMergeEps * 0.5, 0, 0});
    const auto poly = hull(points);
    CHECK(poly.faces.size() == 4);
    sameVertices(poly, original);
    contains(poly, points);
}

TEST_CASE("hull rejects empty collinear coplanar and nonfinite clouds") {
    CHECK_FALSE(convexHull({}));
    const std::vector<DVec3> plane{{0, 0, 0}, {64, 0, 0}, {64, 64, 0}, {0, 64, 0}};
    CHECK_FALSE(convexHull(plane));
    const std::vector<DVec3> line{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}};
    CHECK_FALSE(convexHull(line));
    CHECK_FALSE(convexHull(std::span(plane).first(3)));
    auto invalid = plane;
    invalid[0].z = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(convexHull(invalid));
}

TEST_CASE("diagonal cube split preserves faces and identifies default caps") {
    const auto cube = build(boxSides());
    const Plane plane{glm::normalize(DVec3{1, 1, 1}), 0};
    const auto cut = split(cube, plane);
    REQUIRE(cut.front);
    REQUIRE(cut.back);
    REQUIRE(cut.frontCap);
    REQUIRE(cut.backCap);
    for (const auto* piece : {&*cut.front, &*cut.back}) {
        CHECK(piece->isValid());
        CHECK(piece->faces.size() == 7);
        for (const auto& face : piece->faces) {
            if (face.isCap) {
                sameAttributes(face, FaceAttributes{});
                CHECK(face.vertices.size() == 6);
            } else {
                const auto old = std::find_if(cube.faces.begin(), cube.faces.end(),
                    [&](const Face& f) { return f.sideId == face.sideId; });
                REQUIRE(old != cube.faces.end());
                sameAttributes(face, *old);
            }
        }
    }
    CHECK(cut.front->faces[*cut.frontCap].isCap);
    CHECK(cut.back->faces[*cut.backCap].isCap);
    CHECK(glm::dot(cut.front->faces[*cut.frontCap].plane.normal, plane.normal) < -0.999);
    CHECK(glm::dot(cut.back->faces[*cut.backCap].plane.normal, plane.normal) > 0.999);
    CHECK(cut.front->volume() + cut.back->volume() == doctest::Approx(cube.volume()));
}

TEST_CASE("split classifies outside tangent and existing edge planes") {
    const auto cube = build(boxSides());
    for (const double distance : {64.0, 65.0}) {
        const auto cut = split(cube, {{1, 0, 0}, distance});
        CHECK_FALSE(cut.front);
        REQUIRE(cut.back);
        CHECK_FALSE(cut.backCap);
        sameVertices(*cut.back, cube);
        CHECK(cut.back->faces.size() == cube.faces.size());
    }
    CHECK_FALSE(split(cube, {{1, 0, 0}, -65}).back);
    const auto edgeCut = split(cube, {glm::normalize(DVec3{1, 1, 0}), 0});
    REQUIRE(edgeCut.front);
    REQUIRE(edgeCut.back);
    CHECK(edgeCut.front->isValid());
    CHECK(edgeCut.back->isValid());
    CHECK(edgeCut.front->volume() + edgeCut.back->volume() == doctest::Approx(cube.volume()));
    CHECK_THROWS_AS(split(cube, {{0, 0, 0}, 0}), std::invalid_argument);
}

TEST_CASE("subtract makes a doorway notch with ordered attributed cap provenance") {
    const auto wall = build(boxSides({-128, -16, 0}, {128, 16, 128}));
    const auto door = build(boxSides({-32, -32, -16}, {32, 32, 96}));
    const auto fragments = subtract(wall, door);
    REQUIRE(fragments.size() == 3);
    CHECK(totalVolume(fragments) == doctest::Approx(wall.volume() - 64.0 * 32.0 * 96.0));
    const std::array<std::size_t, 3> cutOrder{0, 1, 4};
    for (std::size_t i = 0; i < fragments.size(); ++i) {
        const auto& fragment = fragments[i];
        CHECK(fragment.isValid());
        bool ownCap = false;
        for (const auto& face : fragment.faces) {
            if (!face.isCap) continue;
            REQUIRE(face.carverFace);
            CHECK(*face.carverFace <= cutOrder[i]);
            ownCap |= *face.carverFace == cutOrder[i];
            CHECK(std::abs(glm::dot(face.plane.normal,
                                   door.faces[*face.carverFace].plane.normal)) > 0.999);
            sameAttributes(face, FaceAttributes{});
        }
        CHECK(ownCap);
    }
}

TEST_CASE("subtract disjoint touching containing and contained boxes") {
    const auto cube = build(boxSides());
    for (const auto lo : {DVec3{96, 0, 0}, DVec3{64, 0, 0}, DVec3{-96, -96, 96}}) {
        const auto pieces = subtract(cube, build(boxSides(lo, lo + DVec3{32})));
        REQUIRE(pieces.size() == 1);
        sameVertices(pieces[0], cube);
        for (std::size_t i = 0; i < cube.faces.size(); ++i)
            sameAttributes(pieces[0].faces[i], cube.faces[i]);
    }
    CHECK(subtract(cube, cube).empty());
    CHECK(subtract(cube, build(boxSides(DVec3{-128}, DVec3{128}))).empty());
    const auto inner = build(boxSides(DVec3{-32}, DVec3{32}));
    const auto pieces = subtract(cube, inner);
    CHECK(pieces.size() == 6);
    CHECK(totalVolume(pieces) + inner.volume() == doctest::Approx(cube.volume()));
}

TEST_CASE("new solid writer uses Hammer key order placeholders and Strata point data") {
    const auto cube = build(boxSides());
    const NewSolidStyle style{"brick/chosen", 0.5, 32};
    const auto block = geometry::solidBlock(cube, style);
    CHECK(block.key() == "solid");
    CHECK(block.children().front().key() == "id");
    CHECK(block.find("id")->asInt().value() == 0);
    const std::array<std::string_view, 9> keys{"id", "plane", "point_data", "material", "uaxis", "vaxis",
                                            "rotation", "lightmapscale", "smoothing_groups"};
    for (const auto* side : block.findAll("side")) {
        REQUIRE(side->children().size() == keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) CHECK(side->children()[i].key() == keys[i]);
        CHECK(side->find("id")->asInt().value() == 0);
        CHECK(side->find("material")->decodedValue().value() == style.material);
        CHECK(side->find("rotation")->asInt().value() == 0);
        CHECK(side->find("point_data")->find("numpts")->asInt().value() == 4);
    }
    kv1::Document document;
    document.root().append(block).value();
    const auto encoded = kv1::write(document).value();
    const auto decoded = kv1::parse(encoded).value();
    const auto read = solidGeometry(*decoded.root().find("solid"));
    REQUIRE(read);
    CHECK_FALSE(read->usedPlaneFallback);
    sameVertices(read->poly, cube);
    CHECK(read->poly.faces.size() == 6);
    for (std::size_t i = 0; i < read->poly.faces.size(); ++i) {
        const auto& face = read->poly.faces[i];
        const auto world = generateUv(cube.faces[i], 1, 1);
        checkVector(face.uv.uaxis, world.uaxis);
        checkVector(face.uv.vaxis, world.vaxis);
        CHECK(face.uv.scaleU == style.textureScale);
        CHECK(face.uv.scaleV == style.textureScale);
        CHECK(face.lightmapScale == style.lightmapScale);
    }
}

TEST_CASE("new solid face alignment lies in slope planes world uses dominant axes") {
    auto sides = boxSides({0, 0, 0}, {64, 64, 64});
    sides.push_back(side(glm::normalize(DVec3{1, 0, 1}), 64 / std::sqrt(2.0), 99));
    const auto slope = build(sides);
    for (const auto alignment : {UvAlignment::Face, UvAlignment::World}) {
        const auto parsed = solidGeometry(geometry::solidBlock(slope, {"tools/test", 0.25, 16, alignment}));
        REQUIRE(parsed);
        CHECK_FALSE(parsed->usedPlaneFallback);
        for (const auto& face : parsed->poly.faces) {
            if (alignment == UvAlignment::Face) {
                CHECK(std::abs(glm::dot(face.uv.uaxis, face.plane.normal)) < 1e-12);
                CHECK(std::abs(glm::dot(face.uv.vaxis, face.plane.normal)) < 1e-12);
                CHECK(std::abs(glm::dot(face.uv.uaxis, face.uv.vaxis)) < 1e-12);
                CHECK(glm::length(face.uv.uaxis) == doctest::Approx(1.0));
                CHECK(glm::length(face.uv.vaxis) == doctest::Approx(1.0));
            } else {
                const auto world = generateUv(face, 1, 1);
                checkVector(face.uv.uaxis, world.uaxis);
                checkVector(face.uv.vaxis, world.vaxis);
            }
        }
    }
    const auto precise = build(offGridSides());
    const auto roundTrip = solidGeometry(geometry::solidBlock(precise, {"test/material"}));
    REQUIRE(roundTrip);
    CHECK_FALSE(roundTrip->usedPlaneFallback);
    sameVertices(roundTrip->poly, precise);
    for (std::size_t i = 0; i < precise.vertices.size(); ++i)
        CHECK(std::find(roundTrip->poly.vertices.begin(), roundTrip->poly.vertices.end(),
                        precise.vertices[i]) != roundTrip->poly.vertices.end());
}

TEST_CASE("fixed seed hull split subtract properties near Source coordinate limits") {
    std::mt19937_64 random(0x031d8);
    std::uniform_real_distribution<double> coordinate(-65536.0, 65536.0);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    for (int iteration = 0; iteration < 80; ++iteration) {
        CAPTURE(iteration);
        const DVec3 center{coordinate(random), coordinate(random), coordinate(random)};
        const double radius = iteration % 2 == 0 ? 16.0 : 8192.0;
        std::vector<DVec3> points;
        for (int i = 0; i < 24; ++i)
            points.push_back(center + DVec3{unit(random), unit(random), unit(random)} * radius);
        const auto poly = hull(points);
        CHECK(poly.isValid());
        contains(poly, points);
        const Plane plane{glm::normalize(DVec3{unit(random), unit(random), unit(random)}), 0};
        auto cutPlane = plane;
        cutPlane.dist = glm::dot(plane.normal, center + DVec3{unit(random)} * radius * 0.5);
        const auto cut = split(poly, cutPlane);
        double volume = 0;
        for (const auto* piece : {&cut.front, &cut.back}) {
            if (!*piece) continue;
            CHECK((*piece)->isValid());
            volume += (*piece)->volume();
        }
        CHECK(volume == doctest::Approx(poly.volume()).epsilon(1e-8));
        const auto carver = build(boxSides(center - DVec3{radius * 0.5},
                                           center + DVec3{radius * 0.5}));
        const auto fragments = subtract(poly, carver);
        for (const auto& fragment : fragments) CHECK(fragment.isValid());
        const auto overlap = intersection(poly, carver);
        const double conserved = totalVolume(fragments) + (overlap ? overlap->volume() : 0.0);
        CHECK(conserved == doctest::Approx(poly.volume()).epsilon(1e-8));
    }
}

TEST_CASE("long sliver split conserves volume within area times weld distance") {
    // hull_fuzz regression: the error scales with surface area, not with volume.
    const std::vector<DVec3> points{{62194.94836425781, -20046.310180664062, -3.0517578125e-05},
                                    {0.000152587890625, 0, 0},
                                    {0, 0, -0.304718017578125},
                                    {0.046844482421875, 0, 5362.949005126953}};
    const auto poly = hull(points);
    contains(poly, points);
    double area = 0.0;
    for (const auto& face : poly.faces) area += poly.faceArea(face);
    const auto normal = glm::normalize(points.front() - points.back());
    const auto cut = split(poly, {normal, glm::dot(normal, poly.center())});
    REQUIRE(cut.front);
    REQUIRE(cut.back);
    CHECK(cut.front->isValid());
    CHECK(cut.back->isValid());
    CHECK(std::abs(cut.front->volume() + cut.back->volume() - poly.volume()) <=
          area * kPointMergeEps);
}
