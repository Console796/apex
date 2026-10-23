#include "geometry/convex_polyhedron.hpp"

#include "geometry/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <iterator>

namespace apex::geometry {
namespace {

struct Triangle {
    std::array<std::size_t, 3> vertices;
    Plane plane;
    std::vector<std::size_t> outside;
};

using Edge = std::pair<std::size_t, std::size_t>;
void boundaryEdge(std::map<Edge, bool>& edges, std::size_t a, std::size_t b) {
    if (!edges.erase({b, a})) edges.emplace(Edge{a, b}, true);
}

std::unexpected<Problems> degenerate() {
    return std::unexpected(Problems{{ProblemCode::DegenerateVolume,
                                    "Hull needs four non-coplanar points above kernel tolerances"}});
}

} // namespace

std::expected<ConvexPolyhedron, Problems> convexHull(std::span<const DVec3> input) {
    std::vector<DVec3> points;
    for (const auto p : input) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return std::unexpected(Problems{{ProblemCode::InvalidVertex, "Nonfinite hull point"}});
        if (std::none_of(points.begin(), points.end(), [p](DVec3 q) {
                return glm::length(p - q) <= kPointMergeEps;
            })) points.push_back(p);
    }
    if (points.size() < 4) return degenerate();

    // A wide initial simplex keeps the interior reference away from the hull boundary.
    std::size_t a = 0, b = 0, c = 0, d = 0;
    double longest = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        const auto [lo, hi] = std::minmax_element(points.begin(), points.end(),
            [axis](DVec3 p, DVec3 q) { return p[axis] < q[axis]; });
        const double distance = glm::length(*hi - *lo);
        if (distance > longest) {
            longest = distance;
            a = static_cast<std::size_t>(lo - points.begin());
            b = static_cast<std::size_t>(hi - points.begin());
        }
    }
    if (longest < kMinEdgeLength) return degenerate();
    double widest = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double distance = glm::length(glm::cross(points[b] - points[a],
                                                        points[i] - points[a])) / longest;
        if (distance > widest) { widest = distance; c = i; }
    }
    if (widest < kMinEdgeLength) return degenerate();
    const auto base = Plane::fromPoints(points[a], points[b], points[c]);
    double highest = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double distance = std::abs(base.signedDistance(points[i]));
        if (distance > highest) { highest = distance; d = i; }
    }
    if (highest <= kPlanarEps) return degenerate();
    const auto interior = points[a] * 0.25 + points[b] * 0.25 +
                          points[c] * 0.25 + points[d] * 0.25;
    const auto triangle = [&](std::size_t i, std::size_t j, std::size_t k) {
        Triangle t{{i, j, k}, Plane::fromPoints(points[i], points[j], points[k]), {}};
        if (t.plane.signedDistance(interior) > 0.0) {
            std::swap(t.vertices[1], t.vertices[2]);
            t.plane = Plane::fromPoints(points[i], points[k], points[j]);
        }
        return t;
    };
    std::vector<Triangle> hull{triangle(a, b, c), triangle(a, d, b),
                               triangle(b, d, c), triangle(c, d, a)};
    const auto assign = [&](std::size_t point, std::vector<Triangle>& faces) {
        double distance = kPlanarEps;
        Triangle* best = nullptr;
        for (auto& face : faces) {
            const double candidate = face.plane.signedDistance(points[point]);
            if (candidate > distance) { distance = candidate; best = &face; }
        }
        if (best) best->outside.push_back(point);
    };
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i != a && i != b && i != c && i != d) assign(i, hull);
    }
    for (;;) {
        const auto active = std::find_if(hull.begin(), hull.end(),
                                        [](const Triangle& t) { return !t.outside.empty(); });
        if (active == hull.end()) break;
        const auto eye = *std::max_element(active->outside.begin(), active->outside.end(),
            [&](std::size_t i, std::size_t j) {
                return active->plane.signedDistance(points[i]) <
                       active->plane.signedDistance(points[j]);
            });
        std::map<Edge, bool> horizon;
        std::vector<std::size_t> orphaned;
        std::vector<Triangle> retained;
        for (auto& face : hull) {
            if (face.plane.signedDistance(points[eye]) > kPlanarEps) {
                for (std::size_t i = 0; i < 3; ++i)
                    boundaryEdge(horizon, face.vertices[i], face.vertices[(i + 1) % 3]);
                for (const auto i : face.outside) if (i != eye) orphaned.push_back(i);
            } else retained.push_back(std::move(face));
        }
        std::vector<Triangle> added;
        for (const auto& [edge, unused] : horizon) {
            (void)unused;
            added.push_back(triangle(edge.first, edge.second, eye));
        }
        for (const auto i : orphaned) assign(i, added);
        retained.insert(retained.end(), std::make_move_iterator(added.begin()),
                        std::make_move_iterator(added.end()));
        hull = std::move(retained);
    }

    // Cancel internal triangle edges; keep shared collinear boundary subdivisions.
    std::vector<bool> merged(hull.size(), false);
    ConvexPolyhedron result;
    result.vertices = points;
    for (std::size_t i = 0; i < hull.size(); ++i) {
        if (merged[i]) continue;
        std::map<Edge, bool> boundary;
        for (std::size_t j = i; j < hull.size(); ++j) {
            if (merged[j] || glm::length(hull[i].plane.normal - hull[j].plane.normal) >
                                 kNormalMatchEps) continue;
            if (std::any_of(hull[j].vertices.begin(), hull[j].vertices.end(),
                [&](std::size_t v) {
                    return std::abs(hull[i].plane.signedDistance(points[v])) > kPlanarEps;
                })) continue;
            merged[j] = true;
            for (std::size_t k = 0; k < 3; ++k)
                boundaryEdge(boundary, hull[j].vertices[k], hull[j].vertices[(k + 1) % 3]);
        }
        if (boundary.empty()) return degenerate();
        Face face;
        auto next = boundary.begin()->first.first;
        const auto first = next;
        while (!boundary.empty()) {
            const auto found = boundary.lower_bound({next, 0});
            if (found == boundary.end() || found->first.first != next) return degenerate();
            face.vertices.push_back(next);
            next = found->first.second;
            boundary.erase(found);
        }
        if (next != first) return degenerate();
        const auto plane = result.derivePlane(face);
        if (!plane) return degenerate();
        face.plane = *plane;
        result.faces.push_back(std::move(face));
    }
    std::vector<DVec3> used;
    std::map<std::size_t, std::size_t> remap;
    for (auto& face : result.faces) {
        for (auto& v : face.vertices) {
            const auto [it, inserted] = remap.emplace(v, used.size());
            if (inserted) used.push_back(points[v]);
            v = it->second;
        }
    }
    result.vertices = std::move(used);
    auto problems = result.validate();
    for (const auto& face : result.faces) {
        if (std::any_of(input.begin(), input.end(), [&](DVec3 p) {
                return face.plane.signedDistance(p) > kPlanarEps;
            })) problems.push_back({ProblemCode::NonConvex, "Hull does not contain every input point"});
    }
    if (!problems.empty()) return std::unexpected(std::move(problems));
    return result;
}

} // namespace apex::geometry
