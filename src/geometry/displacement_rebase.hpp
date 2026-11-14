#pragma once

#include "core/math.hpp"
#include "core/result.hpp"
#include "geometry/displacement.hpp"

#include <array>

namespace apex::geometry {

// Plan 0.8 D5: re-base a displacement when its base face moves by an affine map, so that the
// mesh follows the face (the vertex tool uses this). oldCorners and newCorners are the base face's
// four corners before and after, in corresponding order (newCorners[k] is where oldCorners[k]
// went). Three corners fix the map in the face's plane; out of the plane the old unit normal maps
// to the new unit normal, so heights above the face are kept. The map is applied with
// transformDisplacement (D4). An error when the fourth corner is not the affine image of the
// others (the quad bends or skews non-affinely), when either quad is degenerate, or when the
// displacement is invalid.
Result<Displacement> rebaseDisplacement(const std::array<DVec3, 4>& oldCorners,
                                        const std::array<DVec3, 4>& newCorners, const Displacement& disp);

} // namespace apex::geometry
