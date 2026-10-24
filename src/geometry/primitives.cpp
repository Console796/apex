#include "geometry/primitives.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace apex::geometry {
std::expected<std::vector<ConvexPolyhedron>, Problems>
primitive(const PrimitiveBox& box, const PrimitiveParameters& p) {
    const auto invalid = []() -> std::expected<std::vector<ConvexPolyhedron>, Problems> {
        return std::unexpected(Problems{{ProblemCode::InvalidVertex, "Invalid primitive dimensions or parameters"}});
    };
    const auto lo = box.bounds.min, hi = box.bounds.max;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(lo[i]) || !std::isfinite(hi[i]) || hi[i] <= lo[i]) return invalid();
        if (!std::isfinite(box.origin[i])) return invalid();
        for (int j = 0; j < 3; ++j)
            if (!std::isfinite(box.axes[i][j])) return invalid();
        for (int j = 0; j < 3; ++j)
            if (std::abs(glm::dot(box.axes[i], box.axes[j]) - (i == j ? 1.0 : 0.0)) > 1e-6) return invalid();
    }
    const auto centre = (lo + hi) * 0.5, radius = (hi - lo) * 0.5;
    constexpr double pi = std::numbers::pi;
    std::vector<ConvexPolyhedron> result;
    const auto hull = [&](std::vector<DVec3> points) -> bool {
        // Build near zero before moving to map coordinates, including far-corner draws.
        auto poly = convexHull(points);
        if (!poly) return false;
        for (auto& v : poly->vertices) v = box.origin + box.axes[0] * v.x + box.axes[1] * v.y + box.axes[2] * v.z;
        for (auto& f : poly->faces) {
            const auto plane = poly->derivePlane(f);
            if (!plane) return false;
            f.plane = *plane;
        }
        if (!poly->isValid()) return false;
        result.push_back(std::move(*poly));
        return true;
    };
    const auto ringPoint = [&](double angle, double x, double y, double z) {
        return DVec3{centre.x + x * std::cos(angle), centre.y + y * std::sin(angle), z};
    };
    switch (p.type) {
    case PrimitiveType::Wedge:
        if (!hull({lo, {hi.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z},
                   {lo.x, hi.y, hi.z}, hi})) return invalid();
        break;
    case PrimitiveType::Cylinder:
    case PrimitiveType::Spike: {
        if (p.sides < 3 || p.sides > 64) return invalid();
        std::vector<DVec3> points;
        for (int i = 0; i < p.sides; ++i) {
            const double a = 2.0 * pi * i / p.sides;
            points.push_back(ringPoint(a, radius.x, radius.y, lo.z));
            if (p.type == PrimitiveType::Cylinder) points.push_back(ringPoint(a, radius.x, radius.y, hi.z));
        }
        if (p.type == PrimitiveType::Spike) points.push_back({centre.x, centre.y, hi.z});
        if (!hull(std::move(points))) return invalid();
        break;
    }
    case PrimitiveType::Arch: {
        if (p.sides < 3 || p.sides > 100 || !std::isfinite(p.arcAngle) || p.arcAngle < 8.0 || p.arcAngle > 360.0 ||
            !std::isfinite(p.startAngle) || !std::isfinite(p.wallWidth) || p.wallWidth <= 0.0 ||
            p.wallWidth >= std::min(radius.x, radius.y) || !std::isfinite(p.addHeight)) return invalid();
        // Hammer's add height raises each successive segment by that much; segments keep the drawn height.
        for (int i = 0; i < p.sides; ++i) {
            std::vector<DVec3> points;
            for (int end = 0; end < 2; ++end) {
                const double a = (p.startAngle + p.arcAngle * (i + end) / p.sides) * pi / 180.0;
                const double segmentBottom = lo.z + p.addHeight * i;
                const double segmentTop = hi.z + p.addHeight * i;
                for (double z : {segmentBottom, segmentTop}) {
                    points.push_back(ringPoint(a, radius.x, radius.y, z));
                    points.push_back(ringPoint(a, radius.x - p.wallWidth, radius.y - p.wallWidth, z));
                }
            }
            if (!hull(std::move(points))) return invalid();
        }
        break;
    }
    case PrimitiveType::Sphere:
        if (p.segments < 3 || p.segments > 64) return invalid();
        // Latitude frusta share their end rings; pole bands are pyramids.
        for (int band = 0; band < p.segments; ++band) {
            std::vector<DVec3> points;
            for (int end = 0; end < 2; ++end) {
                const double latitude = -pi * 0.5 + pi * (band + end) / p.segments;
                const double z = centre.z + radius.z * std::sin(latitude);
                if (band + end == 0 || band + end == p.segments) points.push_back({centre.x, centre.y, z});
                else for (int i = 0; i < p.segments; ++i)
                    points.push_back(ringPoint(2.0 * pi * i / p.segments, radius.x * std::cos(latitude),
                                              radius.y * std::cos(latitude), z));
            }
            if (!hull(std::move(points))) return invalid();
        }
        break;
    default: return invalid();
    }
    return result;
}
} // namespace apex::geometry
