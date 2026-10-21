#pragma once

// What tools see of a viewport (plan 0.1, D15): a cursor ray, projection kind, view direction,
// the active work plane, and grid/snap settings. Tools branch on the WorkPlane policy, never on a
// "2D view" flag. Built and tested in 0.1; first consumed by tools in 0.2.

#include "viewport/viewport.hpp"

#include <optional>

namespace apex::viewport {

struct WorkPlane {
    DVec3 origin{0.0};
    DVec3 normal{0.0, 0.0, 1.0}; // Unit length.
    DVec3 uAxis{1.0, 0.0, 0.0};  // In-plane axes, unit and orthogonal to each other and normal.
    DVec3 vAxis{0.0, 1.0, 0.0};

    // Ray parameter t of the intersection, or nullopt when parallel (or behind, unless
    // `allowBehind`; ortho rays start on the eye plane, so their useful hits can have t < 0).
    std::optional<double> intersect(const Ray& ray, bool allowBehind = false) const;
    DVec3 project(DVec3 point) const; // Orthogonal projection onto the plane.
    // Plane coordinates of a point on the plane.
    DVec2 coordinates(DVec3 point) const;
};

struct GridSettings {
    double spacing = 64.0;
    bool snap = true;
    double snapValue(double v) const;
    DVec3 snapPoint(DVec3 p) const;
};

struct ViewContext {
    Ray cursorRay;
    ProjectionKind projection = ProjectionKind::Perspective;
    ViewAxis axis = ViewAxis::Free;
    DVec3 viewDirection{1.0, 0.0, 0.0};
    WorkPlane workPlane;
    GridSettings grid;
    DVec2 cursor{0.0};       // Pixels, y down.
    DVec2 viewportSize{1.0};

    bool axisLocked() const { return axis != ViewAxis::Free; }
};

// Perspective and free cameras get the ground plane (z = `groundZ`). Axis-locked cameras get a
// plane facing the camera, positioned at the selection bounds' centre when there is a selection,
// else at the camera position (its depth along the view axis is the policy hook for 0.2's
// "new brush depth from the selection's bounds").
WorkPlane makeWorkPlane(const Camera& camera, const std::optional<AABB>& selection = std::nullopt,
                        double groundZ = 0.0);

// Ray through a pixel (y down). Perspective rays start at the eye; ortho rays start on the
// image plane through the camera position, so t = 0 is the eye plane.
Ray cursorRay(const Camera& camera, DVec2 cursor, DVec2 viewportSize);

ViewContext makeViewContext(const Viewport& viewport, DVec2 cursor, DVec2 viewportSize,
                            const GridSettings& grid = {},
                            const std::optional<AABB>& selection = std::nullopt);

} // namespace apex::viewport
