#pragma once

#include "viewport/camera.hpp"

#include <array>

namespace apex::viewport {

// Top-left pixel coordinates in the same units as width/height (logical or physical).
// Perspective starts at the eye; ortho starts on the renderer's rear depth boundary.
Ray pixelRay(const Camera& camera, double aspect, double x, double y, double width, double height);
// Four outward side planes, inside <= 0. Rect order is immaterial; a zero extent
// is a closed, zero-width slab. Depth is unbounded, as in PickIndex's marquee query.
std::array<Plane, 4> rectFrustum(const Camera& camera, double aspect,
                               double x0, double y0, double x1, double y1,
                               double width, double height);

} // namespace apex::viewport
