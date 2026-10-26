#pragma once

#include "core/kv1.hpp"
#include "geometry/convex_polyhedron.hpp"

namespace apex::geometry {

enum class UvAlignment { Face, World };

struct NewSolidStyle {
    std::string material;
    double textureScale = 0.25;
    int lightmapScale = 16;
    UvAlignment alignment = UvAlignment::Face;
};

// New-solid defaults apply to every face. IDs are zero placeholders for Transaction::create.
// Invalid geometry or nonpositive/nonfinite style scales throw invalid_argument.
kv1::Node solidBlock(const ConvexPolyhedron& poly, const NewSolidStyle& style);

} // namespace apex::geometry
