#include "viewport/camera.hpp"

#include "render/view_math.hpp"

#include <string_view>

namespace apex::viewport {

double sourceFovYDegrees(double fovDegrees) {
    return glm::degrees(2.0 * std::atan(std::tan(glm::radians(fovDegrees) * 0.5) / (4.0 / 3.0)));
}

DVec2 Camera::halfExtents(double aspect, double depth) const {
    if (isOrtho()) {
        return {orthoHeight * 0.5 * aspect, orthoHeight * 0.5};
    }
    const double halfH = std::tan(glm::radians(fovYDegrees) * 0.5) * depth;
    return {halfH * aspect, halfH};
}

Mat4 Camera::viewMatrix() const {
    return render::rotationOnlyView(basis());
}

Mat4 Camera::projectionMatrix(double aspect, double nearZ, double farZ) const {
    if (isOrtho()) {
        const auto half = halfExtents(aspect);
        return render::reversedZOrtho(half.x, half.y, -farZ, farZ);
    }
    return render::reversedZPerspective(glm::radians(fovYDegrees), aspect, nearZ, farZ);
}

std::optional<DVec3> axisDirection(ViewAxis axis) {
    switch (axis) {
    case ViewAxis::PosX: return DVec3{1, 0, 0};
    case ViewAxis::NegX: return DVec3{-1, 0, 0};
    case ViewAxis::PosY: return DVec3{0, 1, 0};
    case ViewAxis::NegY: return DVec3{0, -1, 0};
    case ViewAxis::PosZ: return DVec3{0, 0, 1};
    case ViewAxis::NegZ: return DVec3{0, 0, -1};
    case ViewAxis::Free: break;
    }
    return std::nullopt;
}

std::optional<DVec3> axisAngles(ViewAxis axis) {
    switch (axis) {
    case ViewAxis::PosX: return DVec3{0, 0, 0};
    case ViewAxis::PosY: return DVec3{0, 90, 0};   // Front
    case ViewAxis::NegX: return DVec3{0, 180, 0};  // Side
    case ViewAxis::NegY: return DVec3{0, 270, 0};
    case ViewAxis::NegZ: return DVec3{90, 90, 0};  // Top
    case ViewAxis::PosZ: return DVec3{-90, 90, 0}; // Bottom
    case ViewAxis::Free: break;
    }
    return std::nullopt;
}

void setAxis(Camera& camera, ViewAxis axis) {
    camera.axis = axis;
    if (const auto angles = axisAngles(axis)) {
        camera.angles = *angles;
    }
}

void frameBounds(Camera& camera, const AABB& bounds, double aspect, double margin) {
    if (bounds.empty()) return;
    if (!(aspect > 0.0)) aspect = 1.0;
    const auto centre = bounds.center();
    // A point entity or a thin face still gets a usable view.
    const double radius = std::max(glm::length(bounds.max - bounds.min) * 0.5, 16.0) * margin;
    const auto forward = camera.forward();
    if (camera.isOrtho()) {
        camera.position = centre + forward * glm::dot(camera.position - centre, forward);
        camera.orthoHeight = 2.0 * radius / std::min(aspect, 1.0);
        return;
    }
    const double halfY = glm::radians(camera.fovYDegrees) * 0.5;
    const double half = std::min(halfY, std::atan(std::tan(halfY) * aspect));
    camera.orbitDistance = radius / std::sin(half);
    camera.position = centre - forward * camera.orbitDistance;
}

namespace {
struct AxisName { ViewAxis axis; const char* name; };
constexpr AxisName kAxes[] = {{ViewAxis::Free, "free"}, {ViewAxis::PosX, "+x"},
                              {ViewAxis::NegX, "-x"},   {ViewAxis::PosY, "+y"},
                              {ViewAxis::NegY, "-y"},   {ViewAxis::PosZ, "+z"},
                              {ViewAxis::NegZ, "-z"}};
} // namespace

const char* nameOf(ViewAxis axis) {
    for (const auto& entry : kAxes) {
        if (entry.axis == axis) return entry.name;
    }
    return "free";
}

const char* nameOf(ProjectionKind kind) {
    return kind == ProjectionKind::Orthographic ? "orthographic" : "perspective";
}

std::optional<ViewAxis> viewAxisFromString(std::string_view text) {
    for (const auto& entry : kAxes) {
        if (text == entry.name) return entry.axis;
    }
    return std::nullopt;
}

std::optional<ProjectionKind> projectionFromString(std::string_view text) {
    if (text == "perspective") return ProjectionKind::Perspective;
    if (text == "orthographic") return ProjectionKind::Orthographic;
    return std::nullopt;
}

} // namespace apex::viewport
