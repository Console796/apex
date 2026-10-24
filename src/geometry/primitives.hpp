#pragma once

#include "geometry/convex_polyhedron.hpp"

namespace apex::geometry {

// Bounds are in the orthonormal frame of the drawn base plane.
struct PrimitiveBox {
    DVec3 origin{0.0};
    std::array<DVec3, 3> axes{DVec3{1, 0, 0}, DVec3{0, 1, 0}, DVec3{0, 0, 1}};
    AABB bounds;
};
enum class PrimitiveType { Wedge, Cylinder, Spike, Arch, Sphere };
struct PrimitiveParameters {
    PrimitiveType type = PrimitiveType::Cylinder;
    int sides = 8;
    double arcAngle = 180.0, startAngle = 0.0, wallWidth = 16.0, addHeight = 0.0;
    int segments = 8;
};

// Invalid parameters or dimensions return problems, never a partial shape.
std::expected<std::vector<ConvexPolyhedron>, Problems>
primitive(const PrimitiveBox& box, const PrimitiveParameters& parameters);

} // namespace apex::geometry
