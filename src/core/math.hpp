#pragma once

// Source QAngles are degrees (pitch, yaw, roll): positive pitch looks DOWN, yaw is CCW
// about +Z from +X, and roll is about forward. Basis columns are forward, right, up.

#include <glm/glm.hpp>

#include <cmath>
#include <limits>

namespace apex {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using DVec2 = glm::dvec2;
using DVec3 = glm::dvec3;
using DVec4 = glm::dvec4;
using DMat3 = glm::dmat3;
using DMat4 = glm::dmat4;

struct AngleVectors {
    DVec3 forward;
    DVec3 right;
    DVec3 up;
};

inline AngleVectors angleVectors(DVec3 anglesDeg) {
    const auto radians = glm::radians(anglesDeg);
    const double cp = std::cos(radians.x), sp = std::sin(radians.x);
    const double cy = std::cos(radians.y), sy = std::sin(radians.y);
    const double cr = std::cos(radians.z), sr = std::sin(radians.z);
    return {{cp * cy, cp * sy, -sp},
            {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp},
            {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp}};
}

// Requires nonzero forward and an up vector not parallel to forward. At vertical forward,
// choose yaw zero and recover the equivalent roll. Inputs need not be unit length.
inline DVec3 anglesFromForward(DVec3 forward, DVec3 up) {
    forward = glm::normalize(forward);
    up = glm::normalize(up - forward * glm::dot(up, forward));
    const double horizontal = std::hypot(forward.x, forward.y);
    const double pitch = std::atan2(-forward.z, horizontal);
    // Machine precision detects the angular singularity, not a geometry tolerance.
    const double yaw = horizontal > std::numeric_limits<double>::epsilon()
                           ? std::atan2(forward.y, forward.x)
                           : 0.0;
    const auto unrolled = angleVectors(glm::degrees(DVec3{pitch, yaw, 0.0}));
    const double roll = std::atan2(glm::dot(up, unrolled.right), glm::dot(up, unrolled.up));
    return glm::degrees(DVec3{pitch, yaw, roll});
}

inline DMat3 basisFromAngles(DVec3 anglesDeg) {
    const auto [forward, right, up] = angleVectors(anglesDeg);
    return {forward, right, up};
}

struct Plane {
    DVec3 normal{0.0, 0.0, 1.0};
    double dist = 0.0; // Plane equation: dot(normal, point) = dist; normal is unit length.

    // Requires three non-collinear points; winding determines the normal.
    static Plane fromPoints(DVec3 a, DVec3 b, DVec3 c) {
        const auto normal = glm::normalize(glm::cross(b - a, c - a));
        return {normal, glm::dot(normal, a)};
    }

    double signedDistance(DVec3 point) const { return glm::dot(normal, point) - dist; }
};

struct AABB {
    DVec3 min{std::numeric_limits<double>::infinity()};
    DVec3 max{-std::numeric_limits<double>::infinity()};

    bool empty() const { return glm::any(glm::greaterThan(min, max)); }
    void expand(DVec3 point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }
    void expand(const AABB& bounds) {
        if (!bounds.empty()) {
            expand(bounds.min);
            expand(bounds.max);
        }
    }
    bool contains(DVec3 point) const {
        return glm::all(glm::greaterThanEqual(point, min)) &&
               glm::all(glm::lessThanEqual(point, max));
    }
    bool contains(const AABB& bounds) const {
        return !bounds.empty() && contains(bounds.min) && contains(bounds.max);
    }
    // Boundaries are inclusive, so touching boxes intersect.
    bool intersects(const AABB& bounds) const {
        return !empty() && !bounds.empty() && glm::all(glm::lessThanEqual(min, bounds.max)) &&
               glm::all(glm::greaterThanEqual(max, bounds.min));
    }
    // Empty boxes have no center or extents; return zero for both.
    DVec3 center() const { return empty() ? DVec3{0.0} : min * 0.5 + max * 0.5; }
    DVec3 extents() const { return empty() ? DVec3{0.0} : max * 0.5 - min * 0.5; }
};

struct Ray {
    DVec3 origin{0.0};
    DVec3 dir{1.0, 0.0, 0.0}; // Need not be normalized; t is a parameter, not necessarily distance.

    DVec3 at(double t) const { return origin + t * dir; }
};

} // namespace apex
