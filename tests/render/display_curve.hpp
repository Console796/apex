#pragma once

#include <algorithm>
#include <cmath>

namespace apex::render::testing {

// Lit's display curve (tonemap.frag.glsl): the piecewise sRGB encode of clamped linear light.
inline double srgbEncode(double linear) {
    linear = std::clamp(linear, 0.0, 1.0);
    return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
}

// Gamma-encoded texture or clear colour to linear (srgbToLinear in common.inc).
inline double srgbDecode(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

// The UNORM8 code Lit writes for a linear value.
inline int displayCode(double linear) { return static_cast<int>(std::lround(255 * srgbEncode(linear))); }

} // namespace apex::render::testing
