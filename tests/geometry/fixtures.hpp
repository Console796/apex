#pragma once

#include "geometry/convex_polyhedron.hpp"
#include "geometry/tolerances.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <utility>

namespace geometry_test {

using namespace apex;
using namespace apex::geometry;

inline PlaneSide side(DVec3 normal, double distance, std::uint32_t id) {
    PlaneSide result;
    result.plane = {normal, distance};
    result.sideId = id;
    result.material = "brick/side_" + std::to_string(id);
    result.uv.uaxis = {1.0, 0.5, -0.25};
    result.uv.vaxis = {0.0, -0.5, 1.0};
    result.uv.shiftU = static_cast<double>(id) + 0.25;
    result.uv.shiftV = -static_cast<double>(id) - 0.5;
    result.uv.scaleU = 0.25;
    result.uv.scaleV = -0.5;
    result.lightmapScale = static_cast<int>(id) + 8;
    result.smoothingGroups = id * 3;
    return result;
}

inline std::vector<PlaneSide> boxSides(DVec3 min = DVec3{-64.0}, DVec3 max = DVec3{64.0}) {
    return {side({1.0, 0.0, 0.0}, max.x, 11), side({-1.0, 0.0, 0.0}, -min.x, 12),
            side({0.0, 1.0, 0.0}, max.y, 13), side({0.0, -1.0, 0.0}, -min.y, 14),
            side({0.0, 0.0, 1.0}, max.z, 15), side({0.0, 0.0, -1.0}, -min.z, 16)};
}

inline std::vector<PlaneSide> wedgeSides() {
    return {side({-1.0, 0.0, 0.0}, 0.0, 1), side({0.0, -1.0, 0.0}, 0.0, 2),
            side({0.0, 0.0, -1.0}, 0.0, 3), side({0.0, 0.0, 1.0}, 32.0, 4),
            side(glm::normalize(DVec3{1.0, 1.0, 0.0}), 64.0 / std::sqrt(2.0), 5)};
}

inline std::vector<PlaneSide> prismSides() {
    std::vector<PlaneSide> result;
    for (std::uint32_t i = 0; i < 16; ++i) {
        const double angle = static_cast<double>(i) * std::numbers::pi / 8.0;
        result.push_back(side({std::cos(angle), std::sin(angle), 0.0}, 64.0, i + 1));
    }
    result.push_back(side({0.0, 0.0, 1.0}, 32.0, 17));
    result.push_back(side({0.0, 0.0, -1.0}, 32.0, 18));
    return result;
}

inline std::vector<PlaneSide> offGridSides() {
    auto result = boxSides({-12.125, -17.75, -3.625}, {31.375, 27.25, 13.875});
    const double c = std::cos(0.37), s = std::sin(0.37);
    const DMat3 yaw{{c, s, 0.0}, {-s, c, 0.0}, {0.0, 0.0, 1.0}};
    const double ct = std::cos(0.21), st = std::sin(0.21);
    const DMat3 tilt{{1.0, 0.0, 0.0}, {0.0, ct, st}, {0.0, -st, ct}};
    for (auto& value : result) {
        value.plane.normal = tilt * yaw * value.plane.normal;
        value.plane.dist += glm::dot(value.plane.normal, DVec3{321.125, -241.375, 51.625});
    }
    return result;
}

inline ConvexPolyhedron build(std::span<const PlaneSide> sides) {
    auto result = ConvexPolyhedron::fromPlanes(sides);
    if (!result) {
        for (const auto& problem : result.error()) {
            MESSAGE(problem.message);
        }
    }
    REQUIRE(result.has_value());
    return std::move(*result);
}

inline void checkVector(DVec3 actual, DVec3 expected) {
    CHECK(glm::length(actual - expected) <= kPlanarEps);
}

inline void sameVertices(const ConvexPolyhedron& actual, const ConvexPolyhedron& expected) {
    REQUIRE(actual.vertices.size() == expected.vertices.size());
    for (const auto point : actual.vertices) {
        std::size_t count = 0;
        for (const auto other : expected.vertices) {
            count += glm::length(point - other) <= kPlanarEps ? 1 : 0;
        }
        CHECK(count == 1);
    }
}

inline void sameAttributes(const FaceAttributes& actual, const FaceAttributes& expected) {
    CHECK(actual.sideId == expected.sideId);
    CHECK(actual.material == expected.material);
    CHECK(actual.uv.uaxis == expected.uv.uaxis);
    CHECK(actual.uv.vaxis == expected.uv.vaxis);
    CHECK(actual.uv.shiftU == expected.uv.shiftU);
    CHECK(actual.uv.shiftV == expected.uv.shiftV);
    CHECK(actual.uv.scaleU == expected.uv.scaleU);
    CHECK(actual.uv.scaleV == expected.uv.scaleV);
    CHECK(actual.lightmapScale == expected.lightmapScale);
    CHECK(actual.smoothingGroups == expected.smoothingGroups);
}

inline std::vector<PlaneSide> rederivedSides(const ConvexPolyhedron& polyhedron) {
    std::vector<PlaneSide> result;
    for (const auto& face : polyhedron.faces) {
        const auto points = polyhedron.sourcePlanePoints(face);
        REQUIRE(points.has_value());
        const auto plane = planePointsFromSource((*points)[0], (*points)[1], (*points)[2]);
        REQUIRE(plane.has_value());
        PlaneSide value;
        value.plane = *plane;
        result.push_back(value);
    }
    return result;
}

inline std::vector<PointDataSide> pointData(const ConvexPolyhedron& polyhedron) {
    std::vector<PointDataSide> result;
    for (const auto& face : polyhedron.faces) {
        PointDataSide value;
        static_cast<FaceAttributes&>(value) = face;
        value.plane = face.plane;
        for (const auto index : face.vertices) {
            value.points.push_back(polyhedron.vertices[index]);
        }
        result.push_back(std::move(value));
    }
    return result;
}

inline bool hasProblem(const std::vector<Problem>& problems, ProblemCode code) {
    for (const auto& problem : problems) {
        if (problem.code == code) {
            return true;
        }
    }
    return false;
}

} // namespace geometry_test
