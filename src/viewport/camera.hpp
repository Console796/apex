#pragma once

// Viewport camera (plan 0.1, D15). Qt-free and GPU-free. World space is Source (Z up, degrees for
// QAngles, see core/math.hpp). The camera is a pure value; controllers mutate it from plain input.

#include "core/math.hpp"

#include <optional>
#include <string_view>

namespace apex::viewport {

// Assumes Source fov is horizontal at 4:3, widened horizontally on wider screens.
double sourceFovYDegrees(double fovDegrees);

enum class ProjectionKind { Perspective, Orthographic };

// Direction the camera looks along when axis-locked. Free leaves the orientation unconstrained.
enum class ViewAxis { Free, PosX, NegX, PosY, NegY, PosZ, NegZ };

// Which controller most recently moved the camera (informational; navigate() picks by input).
enum class ControllerKind { Fly, Orbit, Pan2D };

struct Camera {
    ProjectionKind projection = ProjectionKind::Perspective;
    double fovYDegrees = 75.0;    // Perspective: full vertical field of view.
    double orthoHeight = 1024.0;  // Orthographic: world units visible vertically.
    ViewAxis axis = ViewAxis::Free;
    DVec3 position{0.0};
    DVec3 angles{0.0}; // Source QAngles (pitch, yaw, roll), degrees.
    double orbitDistance = 256.0; // Pivot distance in front of the camera for orbit/dolly.
    ControllerKind controller = ControllerKind::Fly;

    bool isOrtho() const { return projection == ProjectionKind::Orthographic; }
    bool axisLocked() const { return axis != ViewAxis::Free; }
    AngleVectors basis() const { return angleVectors(angles); }
    DVec3 forward() const { return basis().forward; }
    DVec3 pivot() const { return position + forward() * orbitDistance; }

    // Half extents of the visible area on the view plane at `depth` (ortho ignores depth).
    DVec2 halfExtents(double aspect, double depth = 1.0) const;

    // Rotation-only view matrix (render::rotationOnlyView); use camera-relative draw data.
    Mat4 viewMatrix() const;
    // Reversed-Z projection. Ortho uses [-farZ, farZ] depth so geometry behind the eye shows.
    Mat4 projectionMatrix(double aspect, double nearZ = 1.0, double farZ = 65536.0) const;
};

// Unit view direction for an axis lock; Free returns nullopt.
std::optional<DVec3> axisDirection(ViewAxis axis);
// QAngles for an axis lock with the Hammer screen orientation (Top: +X right, +Y up; Front: +X
// right, +Z up looking +Y; Side: +Y right, +Z up looking -X). Free returns nullopt.
std::optional<DVec3> axisAngles(ViewAxis axis);
// Locks the camera to `axis`, snapping its angles. Free just unlocks.
void setAxis(Camera& camera, ViewAxis axis);
// Centres the view on `bounds` without turning it. Perspective backs off along the view direction
// until the bounding sphere fits the narrower field of view, with the orbit pivot at its centre;
// ortho keeps its depth coordinate and fits orthoHeight. Empty bounds leave the camera alone.
void frameBounds(Camera& camera, const AABB& bounds, double aspect, double margin = 1.25);

const char* nameOf(ViewAxis axis);
const char* nameOf(ProjectionKind kind);
std::optional<ViewAxis> viewAxisFromString(std::string_view text);
std::optional<ProjectionKind> projectionFromString(std::string_view text);

} // namespace apex::viewport
