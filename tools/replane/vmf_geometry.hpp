#pragma once

// Test/tool adapter only: the pure geometry layer does not depend on the VMF schema.
#include "document/vmf.hpp"
#include "geometry/convex_polyhedron.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace apex::vmf_geometry {

inline DVec3 vector(vmf::Vec3 value) { return {value[0], value[1], value[2]}; }

template <typename T, typename F> void visitSolids(T& container, const F& visit) {
    for (auto& solid : container.solids) visit(solid);
    for (auto& group : container.groups) visitSolids(group, visit);
    if constexpr (requires { container.hidden; }) {
        for (auto& hidden : container.hidden) visitSolids(hidden, visit);
    }
    if constexpr (requires { container.entities; }) {
        for (auto& entity : container.entities) visitSolids(entity, visit);
    }
}

template <typename F> void visitFile(vmf::VmfFile& file, const F& visit) {
    if (file.world) visitSolids(*file.world, visit);
    for (auto& entity : file.entities) visitSolids(entity, visit);
    for (auto& group : file.groups) visitSolids(group, visit);
    for (auto& hidden : file.hidden) visitSolids(hidden, visit);
}

template <typename T> T require(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().what());
    return std::move(*value);
}

inline std::vector<geometry::PointDataSide> sides(const vmf::Solid& solid) {
    std::vector<geometry::PointDataSide> result;
    for (const auto& side : solid.sides) {
        geometry::PointDataSide value;
        if (!side.plane() || !side.id()) throw std::runtime_error("missing side plane/id");
        const auto points = require(side.plane()->asPlane()).points;
        const auto plane = geometry::planePointsFromSource(vector(points[0]), vector(points[1]),
                                                           vector(points[2]));
        if (!plane) throw std::runtime_error("invalid Source plane points");
        value.plane = *plane;
        value.sideId = static_cast<std::uint32_t>(require(side.id()->asInt()));
        if (side.material()) value.material = require(side.material()->text());
        if (side.uaxis()) {
            const auto axis = require(side.uaxis()->asUvAxis());
            value.uv.uaxis = vector(axis.axis);
            value.uv.shiftU = axis.shift;
            value.uv.scaleU = axis.scale;
        }
        if (side.vaxis()) {
            const auto axis = require(side.vaxis()->asUvAxis());
            value.uv.vaxis = vector(axis.axis);
            value.uv.shiftV = axis.shift;
            value.uv.scaleV = axis.scale;
        }
        if (side.lightmapScale()) value.lightmapScale = require(side.lightmapScale()->asInt());
        if (side.smoothingGroups()) {
            value.smoothingGroups = static_cast<std::uint32_t>(require(side.smoothingGroups()->asInt()));
        }
        if (side.pointData) {
            for (const auto point : require(side.pointData->points())) value.points.push_back(vector(point));
            // VMF point_data follows Source's clockwise plane winding.
            std::reverse(value.points.begin(), value.points.end());
        }
        result.push_back(std::move(value));
    }
    return result;
}

inline std::vector<geometry::PlaneSide> planes(std::span<const geometry::PointDataSide> sides) {
    return {sides.begin(), sides.end()};
}

} // namespace apex::vmf_geometry
