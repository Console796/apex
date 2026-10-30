#pragma once

#include "core/math.hpp"

#include <algorithm>

namespace apex::lighting {

inline DVec3 decodeNormal(DVec3 texel, bool ssbump, bool twoChannel = false) {
    // VDC $ssbump: RGB weights on the three radiosity-normal-map basis directions.
    DVec3 normal = ssbump ?
        texel.r * DVec3(std::sqrt(2.0 / 3.0), 0, std::sqrt(1.0 / 3.0)) +
        texel.g * DVec3(-std::sqrt(1.0 / 6.0), std::sqrt(0.5), std::sqrt(1.0 / 3.0)) +
        texel.b * DVec3(-std::sqrt(1.0 / 6.0), -std::sqrt(0.5), std::sqrt(1.0 / 3.0)) :
        texel * 2.0 - 1.0;
    if (!ssbump && twoChannel) normal.z = std::sqrt(std::max(0.0, 1.0 - normal.x * normal.x - normal.y * normal.y));
    return glm::dot(normal, normal) > 1e-12 ? glm::normalize(normal) : DVec3(0, 0, 1);
}

// Cotangent axes are the projected texture gradients, independently normalized so texel
// density does not change bump strength. Mirrored UVs keep their sign; collapsed UVs are flat.
inline DVec3 perturbNormal(DVec3 normal, DVec3 dpdx, DVec3 dpdy, DVec2 duvdx, DVec2 duvdy,
                           DVec3 tangentNormal) {
    normal = glm::normalize(normal);
    const auto px = glm::cross(dpdy, normal), py = glm::cross(normal, dpdx);
    const double orientation = glm::dot(dpdx, px);
    const double det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (orientation == 0 || det * det <= 1e-12 * glm::dot(duvdx, duvdx) * glm::dot(duvdy, duvdy))
        return normal;
    const double sign = orientation < 0 ? -1.0 : 1.0;
    const auto t = (px * duvdx.x + py * duvdy.x) * sign;
    const auto b = (px * duvdx.y + py * duvdy.y) * sign;
    if (glm::dot(t, t) <= 1e-20 || glm::dot(b, b) <= 1e-20) return normal;
    const auto result = glm::normalize(t) * tangentNormal.x + glm::normalize(b) * tangentNormal.y + normal * tangentNormal.z;
    return glm::dot(result, result) > 1e-12 ? glm::normalize(result) : normal;
}

} // namespace apex::lighting
