#pragma once

#include "core/math.hpp"
#include "core/result.hpp"
#include "geometry/convex_polyhedron.hpp"

#include <optional>

namespace apex::editor {

struct EntityPlacement {
    DVec3 origin;
    std::optional<DVec3> angles;
};
// Align local +Z to the outward normal. Support is computed after rotation, so even an
// asymmetric box rests flush on the plane. A non-unit normal is accepted; zero is invalid.
Result<EntityPlacement> placeOnSurface(DVec3 hit, DVec3 normal, const AABB& bounds, bool align = false);

// The placed box: bounds rotated by the placement's angles (none means axis-aligned) and moved
// to its origin. The Entity tool's ghost and the entity a click creates both come from the same
// placement, so the ghost is exactly where the item lands.
Result<geometry::ConvexPolyhedron> placedBox(const EntityPlacement& placement, const AABB& bounds);

} // namespace apex::editor
