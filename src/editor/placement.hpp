#pragma once

#include "core/math.hpp"
#include "core/result.hpp"

#include <optional>

namespace apex::editor {

struct EntityPlacement {
    DVec3 origin;
    std::optional<DVec3> angles;
};
// Align local +Z to the outward normal. Support is computed after rotation, so even an
// asymmetric box rests flush on the plane. A non-unit normal is accepted; zero is invalid.
Result<EntityPlacement> placeOnSurface(DVec3 hit, DVec3 normal, const AABB& bounds, bool align = false);

} // namespace apex::editor
