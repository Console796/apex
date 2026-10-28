#include "editor/placement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace apex::editor {
Result<EntityPlacement> placeOnSurface(DVec3 hit, DVec3 normal, const AABB& bounds, bool align) {
    const auto finite = [](DVec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!finite(hit) || !finite(normal) || !finite(bounds.min) || !finite(bounds.max) ||
        bounds.empty() || glm::length(normal) <= 0.0 || !std::isfinite(glm::length(normal)))
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid entity placement plane or bounds"});
    normal = glm::normalize(normal);
    EntityPlacement result{hit, {}};
    DMat3 basis{1.0};
    if (align) {
        auto forward = DVec3{1, 0, 0} - normal * normal.x;
        if (glm::length(forward) < 1e-8) forward = DVec3{0, 1, 0} - normal * normal.y;
        result.angles = anglesFromForward(forward, normal);
        basis = basisFromAngles(*result.angles);
    }
    double support = std::numeric_limits<double>::infinity();
    for (unsigned corner = 0; corner < 8; ++corner) {
        const DVec3 point{corner & 1 ? bounds.max.x : bounds.min.x,
                          corner & 2 ? bounds.max.y : bounds.min.y,
                          corner & 4 ? bounds.max.z : bounds.min.z};
        support = std::min(support, glm::dot(normal, basis * point));
    }
    result.origin -= normal * support;
    return result;
}
Result<geometry::ConvexPolyhedron> placedBox(const EntityPlacement& placement, const AABB& bounds) {
    if (bounds.empty()) return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid placement bounds"});
    const DMat3 basis = placement.angles ? basisFromAngles(*placement.angles) : DMat3{1.0};
    std::vector<geometry::PlaneSide> sides;
    for (int axis = 0; axis < 3; ++axis) {
        const DVec3 direction = basis[axis];
        geometry::PlaneSide side;
        side.plane = {direction, glm::dot(direction, placement.origin) + bounds.max[axis]};
        sides.push_back(side);
        side.plane = {-direction, -glm::dot(direction, placement.origin) - bounds.min[axis]};
        sides.push_back(side);
    }
    auto poly = geometry::ConvexPolyhedron::fromPlanes(sides);
    if (!poly) return std::unexpected(Error{ErrorCode::InvalidArgument, "placement bounds are not a solid box"});
    return std::move(*poly);
}
} // namespace apex::editor
