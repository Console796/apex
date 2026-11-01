#pragma once

#include "render/gpu_lighting.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace apex::render {

inline constexpr std::uint32_t kClusterCapacity = 64;
inline constexpr std::uint32_t kClusterSlices = 24;
inline constexpr std::uint32_t kClusterTile = 64;

// std430 scalar array, 260-byte stride. One compute invocation owns one cluster (D14).
struct GpuCluster {
    std::uint32_t count = 0;
    std::uint32_t indices[kClusterCapacity]{};
};
static_assert(sizeof(GpuCluster) == 260);
static_assert(offsetof(GpuCluster, indices) == 4);

inline glm::uvec3 clusterDimensions(VkExtent2D extent) {
    return {std::max(1u, (extent.width + kClusterTile - 1) / kClusterTile),
            std::max(1u, (extent.height + kClusterTile - 1) / kClusterTile), kClusterSlices};
}

// Reversed-Z slices [2^(-z-1), 2^(-z)]; the final slice includes depth zero/infinity.
// Plane/sphere rejection is conservative at corners and for spot cones.
inline std::array<Vec4, 6> clusterPlanes(const Mat4& projection, glm::uvec3 dims, glm::uvec3 cell) {
    const Mat4 rows = glm::transpose(projection);
    const Vec2 lo = Vec2(cell) / Vec2(dims) * 2.0f - 1.0f;
    const Vec2 hi = Vec2(cell + glm::uvec3(1)) / Vec2(dims) * 2.0f - 1.0f;
    const float nearDepth = std::exp2(-float(cell.z));
    const float farDepth = cell.z + 1 == dims.z ? 0.0f : nearDepth * 0.5f;
    return {rows[0] - lo.x * rows[3], hi.x * rows[3] - rows[0],
            rows[1] - lo.y * rows[3], hi.y * rows[3] - rows[1],
            rows[2] - farDepth * rows[3], nearDepth * rows[3] - rows[2]};
}

inline bool clusterIntersects(const std::array<Vec4, 6>& planes, Vec3 origin, float radius) {
    if (radius <= 0) return false;
    if (std::isinf(radius)) return true;
    for (const auto plane : planes)
        if (glm::dot(plane, Vec4(origin, 1)) < -radius * glm::length(Vec3(plane))) return false;
    return true;
}

} // namespace apex::render
