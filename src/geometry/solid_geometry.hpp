#pragma once

#include "core/kv1.hpp"
#include "geometry/convex_polyhedron.hpp"

namespace apex::geometry {

struct SolidGeometry {
    ConvexPolyhedron poly;
    std::vector<PointDataSide> sides; // Original inputs, in source side order.
    bool usedPlaneFallback = false;
};

// Reconcile point_data against the saved planes using the D8 trust rule.
Result<SolidGeometry> solidGeometry(const kv1::Node& solid);

} // namespace apex::geometry
