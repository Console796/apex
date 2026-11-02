#include "lighting/charting.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <climits>
#include <stdexcept>

using namespace apex;

namespace {

geometry::ConvexPolyhedron rectangle(double width = 64, double height = 64, DVec3 origin = DVec3{0}) {
    geometry::ConvexPolyhedron poly;
    poly.vertices = {origin, origin + DVec3{width, 0, 0}, origin + DVec3{width, height, 0},
                     origin + DVec3{0, height, 0}};
    geometry::Face face;
    face.vertices = {0, 1, 2, 3};
    face.plane = {{0, 0, 1}, origin.z};
    face.sideId = 42;
    face.material = "dev/dev_measuregeneric01b";
    face.smoothingGroups = 7;
    poly.faces.push_back(face);
    return poly;
}

lighting::FaceCharting charts(const geometry::ConvexPolyhedron& poly) {
    return lighting::chartFace(poly.faces[0], poly.vertices);
}

void checkMapping(const lighting::LuxelChart& chart, const Plane& plane) {
    for (const auto sample : {DVec2{0, 0}, DVec2{1, 1}, DVec2{3.25, -2.5}}) {
        const auto world = chart.worldAt(sample);
        CHECK(glm::dot(world, plane.normal) == doctest::Approx(plane.dist));
        for (int i = 0; i < 2; ++i)
            CHECK(glm::dot(world, chart.worldToLuxel[i]) + chart.offset[i] - chart.mins[i] ==
                  doctest::Approx(sample[i]));
    }
}

} // namespace

TEST_CASE("lighting chart: 64 unit face has endpoint samples at scales 16 and 4") {
    auto poly = rectangle();
    for (const int scale : {16, 4}) {
        poly.faces[0].lightmapScale = scale;
        const auto result = charts(poly);
        const int extent = 64 / scale;
        CHECK(result.chart.mins == std::array<int, 2>{0, -extent});
        CHECK(result.chart.extents == std::array<int, 2>{extent, extent});
        CHECK(result.chart.dimensions == std::array<int, 2>{extent + 1, extent + 1});
        CHECK(result.chart.origin == DVec3{0, 64, 0});
        REQUIRE(result.fragments.size() == 1);
        CHECK(result.fragments[0].vertices == poly.vertices);
        checkMapping(result.chart, poly.faces[0].plane);
    }
}

TEST_CASE("lighting chart: shifted negative coordinates round outwards") {
    auto poly = rectangle(64, 64, {-1, -3, 8});
    const auto result = charts(poly);
    CHECK(result.chart.mins == std::array<int, 2>{-1, -4});
    CHECK(result.chart.extents == std::array<int, 2>{5, 5});
    CHECK(result.chart.dimensions == std::array<int, 2>{6, 6});
    checkMapping(result.chart, poly.faces[0].plane);
}

TEST_CASE("lighting chart: sloped face with rotated skew texture axes") {
    auto poly = rectangle();
    // z = x + 8; texture axes deliberately neither unit nor orthogonal nor tangent.
    for (auto& p : poly.vertices) p.z = p.x + 8;
    poly.faces[0].plane = *poly.derivePlane(poly.faces[0]);
    poly.faces[0].uv.uaxis = {1, 1, 0};
    poly.faces[0].uv.vaxis = {0, -1, 0};
    const auto result = charts(poly);
    CHECK(result.chart.extents == std::array<int, 2>{8, 4});
    CHECK(result.chart.origin == DVec3{-64, 64, -56});
    checkMapping(result.chart, poly.faces[0].plane);
}

TEST_CASE("lighting chart: texture scale signs affect shifts but not lightmap directions") {
    auto poly = rectangle();
    auto& uv = poly.faces[0].uv;
    uv.scaleU = -0.5;
    uv.scaleV = -2;
    uv.shiftU = 8;
    uv.shiftV = 4;
    const auto result = charts(poly);
    CHECK(result.chart.worldToLuxel[0] == DVec3{1.0 / 16, 0, 0});
    CHECK(result.chart.offset == DVec2{-0.25, -0.5});
    CHECK(result.chart.mins == std::array<int, 2>{-1, -5});
    CHECK(result.chart.extents == std::array<int, 2>{5, 5});
    checkMapping(result.chart, poly.faces[0].plane);
    uv.uaxis = {-1, 0, 0};
    uv.shiftU = uv.shiftV = 0;
    CHECK(charts(poly).chart.mins == std::array<int, 2>{-4, -4});
}

TEST_CASE("lighting chart: VBSP subdivides continuous spans above 128 at minimum plus 127") {
    auto poly = rectangle(128 * 16, 64);
    CHECK(charts(poly).fragments.size() == 1);
    CHECK(charts(poly).chart.dimensions[0] == 129);
    poly.vertices[1].x += 1;
    poly.vertices[2].x += 1;
    const auto result = charts(poly);
    REQUIRE(result.fragments.size() == 2);
    CHECK(result.chart.extents[0] == 129);
    CHECK(result.fragments[0].chart.mins[0] == 127); // front is visited first
    CHECK(result.fragments[0].chart.extents[0] == 2);
    CHECK(result.fragments[1].chart.extents[0] == 127);
    double area = 0;
    for (const auto& fragment : result.fragments) {
        CHECK(fragment.face.sideId == 42);
        CHECK(fragment.face.material == poly.faces[0].material);
        CHECK(fragment.face.smoothingGroups == 7);
        geometry::ConvexPolyhedron piece;
        piece.vertices = fragment.vertices;
        area += piece.faceArea(fragment.face);
        checkMapping(fragment.chart, fragment.face.plane);
    }
    CHECK(area == doctest::Approx(poly.faceArea(poly.faces[0])));
    // Rounding may add another interval without requiring a split.
    poly = rectangle(128 * 16, 64, {1, 0, 0});
    const auto fractional = charts(poly);
    CHECK(fractional.fragments.size() == 1);
    CHECK(fractional.chart.extents[0] == 129);
    CHECK(fractional.chart.dimensions[0] == 130);
}

TEST_CASE("lighting chart: multi-axis splits preserve area and deterministic loops") {
    const auto poly = rectangle(4097, 4103, {-100, -50, 3});
    const auto a = charts(poly), b = charts(poly);
    REQUIRE(a.fragments.size() > 4);
    REQUIRE(a.fragments.size() == b.fragments.size());
    double area = 0;
    for (std::size_t i = 0; i < a.fragments.size(); ++i) {
        const auto& fragment = a.fragments[i];
        CHECK(fragment.vertices == b.fragments[i].vertices);
        CHECK(fragment.chart.mins == b.fragments[i].chart.mins);
        const auto unsplit = lighting::chartFace(fragment.face, fragment.vertices);
        CHECK(unsplit.fragments.size() == 1);
        CHECK(fragment.chart.dimensions[0] <= 130);
        CHECK(fragment.chart.dimensions[1] <= 130);
        geometry::ConvexPolyhedron piece;
        piece.vertices = fragment.vertices;
        area += piece.faceArea(fragment.face);
    }
    CHECK(area == doctest::Approx(poly.faceArea(poly.faces[0])));
}

TEST_CASE("lighting chart: scale edge values follow default VBSP settings") {
    auto poly = rectangle();
    for (const int scale : {0, -1, INT_MIN, 1, 3, INT_MAX}) {
        poly.faces[0].lightmapScale = scale;
        const auto result = charts(poly);
        const double effective = scale == 0 ? 16 : std::max(1, scale);
        CHECK(result.chart.worldToLuxel[0].x == doctest::Approx(1 / effective));
        CHECK(result.chart.extents[0] == static_cast<int>(std::ceil(64 / effective)));
        checkMapping(result.chart, poly.faces[0].plane);
    }
}

TEST_CASE("lighting chart: rejects malformed inputs without mutating the face") {
    auto poly = rectangle();
    poly.faces[0].vertices[0] = 99;
    CHECK_THROWS_AS(charts(poly), std::invalid_argument);
    poly = rectangle();
    poly.faces[0].uv.vaxis = poly.faces[0].uv.uaxis;
    CHECK_THROWS_AS(charts(poly), std::invalid_argument);
    poly = rectangle();
    poly.vertices[0].x = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(charts(poly), std::invalid_argument);
    poly = rectangle(64, 64, {1e20, 0, 0});
    CHECK_THROWS_AS(charts(poly), std::invalid_argument);
}
