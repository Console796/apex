#include "geometry/convex_polyhedron.hpp"
#include "geometry/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
void require(bool condition) { if (!condition) std::abort(); }
// Vertices may move by the weld distance, so volume may drift by area times that distance.
double volumeTolerance(const apex::geometry::ConvexPolyhedron& poly) {
    double area = 0.0;
    for (const auto& face : poly.faces) area += poly.faceArea(face);
    return std::max(1e-6, area * apex::geometry::kPointMergeEps);
}
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace apex;
    using namespace apex::geometry;
    // Fixed-width coordinates cover Source's range, including sub-grid geometry.
    const auto real = [&]() {
        if (size < 4) return 0.0;
        std::int32_t raw;
        std::memcpy(&raw, data, 4);
        data += 4;
        size -= 4;
        return static_cast<double>(raw) / 32768.0;
    };
    std::vector<DVec3> points;
    while (size >= 12 && points.size() < 32)
        points.push_back({real(), real(), real()});
    auto hull = convexHull(points);
    if (!hull) return 0;
    require(hull->isValid());
    for (const auto& face : hull->faces)
        for (const auto point : points)
            require(face.plane.signedDistance(point) <= kPlanarEps);
    // Derive a non-axial split from the input even when no trailing bytes remain.
    auto normal = points.front() - points.back();
    if (glm::length(normal) < kMinEdgeLength) normal = {1, 1, 1};
    normal = glm::normalize(normal);
    try {
        const auto cut = split(*hull, {normal, glm::dot(normal, hull->center())});
        double volume = 0.0;
        for (const auto* piece : {&cut.front, &cut.back}) {
            if (!*piece) continue;
            require((*piece)->isValid());
            volume += (*piece)->volume();
        }
        require(std::abs(volume - hull->volume()) <= volumeTolerance(*hull));
        std::vector<PlaneSide> sides;
        const auto center = hull->center();
        const auto extent = hull->bounds().extents() * 0.5;
        for (int axis = 0; axis < 3; ++axis) {
            DVec3 n{0};
            n[axis] = 1;
            PlaneSide side;
            side.plane = {n, center[axis] + extent[axis]};
            sides.push_back(side);
            side.plane = {-n, -center[axis] + extent[axis]};
            sides.push_back(side);
        }
        auto carver = ConvexPolyhedron::fromPlanes(sides);
        if (!carver) return 0;
        const auto fragments = subtract(*hull, *carver);
        double conserved = 0.0;
        for (const auto& fragment : fragments) {
            require(fragment.isValid());
            conserved += fragment.volume();
        }
        std::optional<ConvexPolyhedron> overlap = *hull;
        for (const auto& face : carver->faces)
            if (overlap) overlap = split(*overlap, face.plane).back;
        if (overlap) conserved += overlap->volume();
        require(std::abs(conserved - hull->volume()) <= volumeTolerance(*hull));
    } catch (const std::invalid_argument&) {
        // Below-tolerance cuts are refused, as are below-tolerance hulls.
    }
    return 0;
}
