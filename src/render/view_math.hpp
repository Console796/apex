#pragma once

// Pure camera math for the renderer (plan 0.1, D13/0.1.9). No Vulkan, so it is unit-testable
// without a GPU.
//
// Conventions:
//   * World space is Source (X forward-ish, Z up), in double precision.
//   * Camera-relative rebasing: world positions are made relative to the camera in double and only
//     then narrowed to float, so precision stays at the camera however far from the origin it is.
//     The view matrix therefore carries rotation only; draw data uses toCameraRelative().
//   * View space is x right, y down, z forward (into the screen), matching Vulkan clip space.
//   * Reversed-Z: depth 1 at the near plane and 0 at the far plane (or infinity). Use depth
//     compare GREATER_OR_EQUAL and clear depth to 0.

#include "core/math.hpp"

namespace apex::render {

// Storage-buffer ABI: three rows, each holding a linear row and translation (48 bytes).
// Input positions remain draw-local. Compute M * origin - camera in double before narrowing,
// rather than applying a float matrix to an already narrowed world/camera-relative position.
struct PreviewTransform {
    Vec4 rows[3];
};
static_assert(sizeof(PreviewTransform) == 48);

inline PreviewTransform cameraRelativePreview(const DMat4& matrix, DVec3 origin, DVec3 camera) {
    const DVec3 offset = DVec3(matrix * DVec4(origin, 1.0)) - camera;
    PreviewTransform result;
    for (int r = 0; r < 3; ++r)
        result.rows[r] = Vec4(float(matrix[0][r]), float(matrix[1][r]), float(matrix[2][r]), float(offset[r]));
    return result;
}

// CPU reference of preview.inc, using the exact packed data uploaded by Renderer.
inline Vec3 previewPosition(const PreviewTransform& transform, Vec3 local) {
    const Vec4 p(local, 1.0f);
    return {glm::dot(transform.rows[0], p), glm::dot(transform.rows[1], p), glm::dot(transform.rows[2], p)};
}

inline Vec3 previewNormal(const PreviewTransform& transform, Vec3 normal) {
    const Mat3 linear = glm::transpose(Mat3(Vec3(transform.rows[0]), Vec3(transform.rows[1]), Vec3(transform.rows[2])));
    return glm::normalize(glm::transpose(glm::inverse(linear)) * normal);
}

// Position relative to the camera, narrowed to float after the double subtraction.
inline Vec3 toCameraRelative(DVec3 world, DVec3 cameraPosition) {
    return Vec3(world - cameraPosition);
}

// Project in double before wrapping: microgrid phases must survive at +/-65536.
inline DVec2 gridCameraCoordinates(DVec3 cameraPosition, DVec3 origin, DVec3 uAxis, DVec3 vAxis) {
    const DVec3 relative = cameraPosition - origin;
    return {glm::dot(relative, uAxis), glm::dot(relative, vAxis)};
}

// Wrap each line set to its own spacing before narrowing. A fixed 1024 period loses
// microgrid precision when the shader adds the phase and computes line derivatives.
inline Vec2 wrapGridCoordinates(DVec2 coordinates, double period) {
    return Vec2(coordinates - glm::floor(coordinates / period) * period);
}

// Model matrix translation for an object whose origin is `objectOrigin`, camera-relative.
inline Mat4 cameraRelativeTranslation(DVec3 objectOrigin, DVec3 cameraPosition) {
    Mat4 result(1.0f);
    result[3] = Vec4(toCameraRelative(objectOrigin, cameraPosition), 1.0f);
    return result;
}

// Rotation-only view matrix from a Source camera basis (unit vectors, see core/math.hpp
// angleVectors): rows are right, -up, forward.
inline Mat4 rotationOnlyView(DVec3 forward, DVec3 right, DVec3 up) {
    const Vec3 r(right), d(-up), f(forward);
    Mat4 result(1.0f);
    result[0] = Vec4(r.x, d.x, f.x, 0.0f);
    result[1] = Vec4(r.y, d.y, f.y, 0.0f);
    result[2] = Vec4(r.z, d.z, f.z, 0.0f);
    return result;
}

inline Mat4 rotationOnlyView(const AngleVectors& basis) {
    return rotationOnlyView(basis.forward, basis.right, basis.up);
}

// Reversed-Z perspective for view space z forward. farZ <= nearZ (or infinity) gives an
// infinite far plane. fovY is the full vertical field of view in radians; nearZ > 0.
inline Mat4 reversedZPerspective(double fovY, double aspect, double nearZ,
                                 double farZ = std::numeric_limits<double>::infinity()) {
    const double tanHalf = std::tan(fovY * 0.5);
    Mat4 result(0.0f);
    result[0][0] = static_cast<float>(1.0 / (aspect * tanHalf));
    result[1][1] = static_cast<float>(1.0 / tanHalf);
    result[2][3] = 1.0f; // clip.w = view z
    if (std::isinf(farZ) || farZ <= nearZ) {
        result[2][2] = 0.0f;
        result[3][2] = static_cast<float>(nearZ);
    } else {
        result[2][2] = static_cast<float>(-nearZ / (farZ - nearZ));
        result[3][2] = static_cast<float>(nearZ * farZ / (farZ - nearZ));
    }
    return result;
}

// Reversed-Z orthographic, centred on the view axis: x in [-halfWidth, halfWidth],
// y in [-halfHeight, halfHeight] (y down), z in [nearZ, farZ] mapping to depth [1, 0].
inline Mat4 reversedZOrtho(double halfWidth, double halfHeight, double nearZ, double farZ) {
    Mat4 result(1.0f);
    result[0][0] = static_cast<float>(1.0 / halfWidth);
    result[1][1] = static_cast<float>(1.0 / halfHeight);
    result[2][2] = static_cast<float>(-1.0 / (farZ - nearZ));
    result[3][2] = static_cast<float>(farZ / (farZ - nearZ));
    return result;
}

} // namespace apex::render
