#include "geometry/face_matching.hpp"

#include "geometry/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <tuple>

namespace apex::geometry {

MatchResult matchFaces(const ConvexPolyhedron& oldPolyhedron, ConvexPolyhedron& newPolyhedron) {
    struct Candidate {
        std::size_t oldIndex;
        double normalDifference;
        double distanceDifference;
    };
    std::vector<std::vector<Candidate>> candidates(newPolyhedron.faces.size());
    for (std::size_t n = 0; n < newPolyhedron.faces.size(); ++n) {
        const auto& plane = newPolyhedron.faces[n].plane;
        for (std::size_t o = 0; o < oldPolyhedron.faces.size(); ++o) {
            const auto& oldPlane = oldPolyhedron.faces[o].plane;
            const double normal = glm::length(plane.normal - oldPlane.normal);
            const double distance = std::abs(plane.dist - oldPlane.dist);
            if (normal <= kNormalMatchEps && distance <= kPlanarEps) {
                candidates[n].push_back({o, normal, distance});
            }
        }
        std::sort(candidates[n].begin(), candidates[n].end(), [](const auto& a, const auto& b) {
            return std::tie(a.normalDifference, a.distanceDifference, a.oldIndex) <
                   std::tie(b.normalDifference, b.distanceDifference, b.oldIndex);
        });
    }
    // Prefer free candidates, then augment earlier assignments if needed. This avoids a
    // greedy match stealing the sole candidate of a later face near a tolerance boundary.
    std::vector<std::optional<std::size_t>> owners(oldPolyhedron.faces.size());
    std::function<bool(std::size_t, std::vector<bool>&)> assign =
        [&](std::size_t n, std::vector<bool>& visited) {
            for (const auto& candidate : candidates[n]) {
                if (!visited[candidate.oldIndex] && !owners[candidate.oldIndex]) {
                    visited[candidate.oldIndex] = true;
                    owners[candidate.oldIndex] = n;
                    return true;
                }
            }
            for (const auto& candidate : candidates[n]) {
                const auto o = candidate.oldIndex;
                if (visited[o]) {
                    continue;
                }
                visited[o] = true;
                if (assign(*owners[o], visited)) {
                    owners[o] = n;
                    return true;
                }
            }
            return false;
        };
    for (std::size_t n = 0; n < newPolyhedron.faces.size(); ++n) {
        std::vector<bool> visited(oldPolyhedron.faces.size(), false);
        assign(n, visited);
    }
    std::vector<std::optional<std::size_t>> matches(newPolyhedron.faces.size());
    MatchResult result;
    for (std::size_t o = 0; o < owners.size(); ++o) {
        if (owners[o]) {
            matches[*owners[o]] = o;
        } else {
            result.vanishedOld.push_back(o);
        }
    }
    for (std::size_t n = 0; n < matches.size(); ++n) {
        if (matches[n]) {
            const auto o = *matches[n];
            static_cast<FaceAttributes&>(newPolyhedron.faces[n]) = oldPolyhedron.faces[o];
            result.matched.push_back({n, o});
        } else {
            std::optional<std::size_t> inheritFrom;
            double alignment = -2.0; // Unit-normal dot products are in [-1, 1].
            for (std::size_t o = 0; o < oldPolyhedron.faces.size(); ++o) {
                const double dot = glm::dot(newPolyhedron.faces[n].plane.normal,
                                            oldPolyhedron.faces[o].plane.normal);
                if (dot > alignment) {
                    alignment = dot;
                    inheritFrom = o;
                }
            }
            result.unmatchedNew.push_back({n, inheritFrom});
        }
    }
    return result;
}

} // namespace apex::geometry
