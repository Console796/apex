#include "geometry/displacement_rebase.hpp"

#include "geometry/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace apex::geometry {
namespace {

Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }

bool finite(DVec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The quad's frame at corner 0: its two edges and the unit normal, or nullopt when degenerate.
std::optional<DMat3> frame(const std::array<DVec3, 4>& c) {
    const DVec3 u = c[1] - c[0], v = c[3] - c[0];
    const DVec3 n = glm::cross(u, v);
    const double area = glm::length(n);
    if (!(area > kMinFaceArea) || !finite(n)) return std::nullopt;
    return DMat3(u, v, n / area);
}

} // namespace

Result<Displacement> rebaseDisplacement(const std::array<DVec3, 4>& oldCorners,
                                        const std::array<DVec3, 4>& newCorners, const Displacement& disp) {
    if (const auto problem = disp.problem()) return std::unexpected(invalid(*problem));
    if (!std::ranges::all_of(oldCorners, finite) || !std::ranges::all_of(newCorners, finite))
        return std::unexpected(invalid("base face corner is not finite"));
    const auto before = frame(oldCorners), after = frame(newCorners);
    if (!before || !after) return std::unexpected(invalid("the displaced face is degenerate"));
    const DMat3 linear = *after * glm::inverse(*before);
    const DVec3 translation = newCorners[0] - linear * oldCorners[0];
    // Corner 2 is the only one the frame did not use: the change is affine when it follows.
    const DVec3 predicted = linear * oldCorners[2] + translation;
    double extent = 1.0;
    for (const auto& corner : newCorners) extent = std::max(extent, glm::length(corner - newCorners[0]));
    if (glm::distance(predicted, newCorners[2]) > kPointDataTrustEps * std::max(1.0, extent / 1024.0))
        return std::unexpected(invalid("the displaced face would not change by an affine map"));
    try {
        return transformDisplacement(disp, linear, translation);
    } catch (const std::invalid_argument& error) {
        return std::unexpected(invalid(error.what()));
    }
}

} // namespace apex::geometry
