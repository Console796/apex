#include "viewport/camera.hpp"

#include "render/view_math.hpp"

#include <string_view>

namespace apex::viewport {

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
