#include "geometry/hotspot.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <vector>

namespace apex::geometry {
namespace {

Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }

std::uint64_t splitmix(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Candidates this close to the best score are equals.
constexpr double kTie = 1e-6;
// A quarter turn of a rect without the rotate flag must beat its unturned fit by more than this.
constexpr double kTurnPenalty = 1e-3;

struct Candidate {
    std::size_t rect = 0;
    int turns = 0;
    double score = 0.0;
};

} // namespace

Result<HotspotFit> fitHotspot(const FaceUv& current, DVec3 normal, std::span<const DVec3> points,
                              std::span<const HotspotRect> rects, const HotspotFitOptions& options) {
    if (points.size() < 3) return std::unexpected(invalid("hotspot fit needs a face of three or more points"));
    if (!(options.texelScale > 0.0) || !std::isfinite(options.texelScale))
        return std::unexpected(invalid("hotspot texel scale must be positive"));
    auto a = current.uaxis - glm::dot(current.uaxis, normal) * normal;
    if (glm::length(a) < 1e-9) a = current.vaxis - glm::dot(current.vaxis, normal) * normal;
    if (glm::length(a) < 1e-9) return std::unexpected(invalid("the face's texture axes lie along its normal"));
    a = glm::normalize(a);
    auto b = glm::cross(normal, a);
    if (glm::dot(b, current.vaxis) < 0.0) b = -b;

    // Extents along the face frame; a quarter turn swaps them.
    double minA = std::numeric_limits<double>::max(), maxA = -minA, minB = minA, maxB = -minA;
    for (const auto& p : points) {
        minA = std::min(minA, glm::dot(p, a)); maxA = std::max(maxA, glm::dot(p, a));
        minB = std::min(minB, glm::dot(p, b)); maxB = std::max(maxB, glm::dot(p, b));
    }
    const double spanA = maxA - minA, spanB = maxB - minB;
    if (spanA < 1e-9 || spanB < 1e-9) return std::unexpected(invalid("the face has no area"));

    std::vector<Candidate> candidates;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const auto& rect = rects[i];
        if (rect.alt != options.alternate || rect.width() <= 0 || rect.height() <= 0) continue;
        for (int turns = 0; turns < (rect.rotate ? 4 : 2); ++turns) {
            const double w = turns % 2 ? spanB : spanA, h = turns % 2 ? spanA : spanB;
            const double aspect = std::abs(std::log((w / h) / (double(rect.width()) / rect.height())));
            const double size = std::abs(std::log(std::sqrt(w * h) / options.texelScale /
                                                  std::sqrt(double(rect.width()) * rect.height())));
            candidates.push_back({i, turns, 2.0 * aspect + size + (!rect.rotate && turns ? kTurnPenalty : 0.0)});
        }
    }
    if (candidates.empty())
        return std::unexpected(invalid(options.alternate ? "the material has no alternate-group hotspot rects"
                                                         : "the material has no hotspot rects"));
    const double best = std::ranges::min(candidates, {}, &Candidate::score).score;
    std::erase_if(candidates, [&](const Candidate& c) { return c.score > best + kTie; });

    std::uint64_t state = options.seed;
    const auto& chosen = candidates[splitmix(state) % candidates.size()];
    const auto& rect = rects[chosen.rect];
    const bool reflected = rect.reflect && (splitmix(state) & 1) != 0;

    // Turning the texture's frame a quarter counter-clockwise: U takes V's place, V takes -U's.
    DVec3 u = a, v = b;
    for (int i = 0; i < chosen.turns; ++i) std::tie(u, v) = std::pair{v, -u};
    if (reflected) u = -u;
    double minU = std::numeric_limits<double>::max(), maxU = -minU, minV = minU, maxV = -minU;
    for (const auto& p : points) {
        minU = std::min(minU, glm::dot(p, u)); maxU = std::max(maxU, glm::dot(p, u));
        minV = std::min(minV, glm::dot(p, v)); maxV = std::max(maxV, glm::dot(p, v));
    }
    // texel = dot(p, axis) / scale + shift: the face's minimum lands on the rect's minimum, its
    // maximum on the rect's maximum.
    FaceUv uv = current;
    uv.uaxis = u;
    uv.vaxis = v;
    uv.scaleU = (maxU - minU) / rect.width();
    uv.scaleV = (maxV - minV) / rect.height();
    uv.shiftU = rect.x1 - minU / uv.scaleU;
    uv.shiftV = rect.y1 - minV / uv.scaleV;
    return HotspotFit{uv, chosen.rect, chosen.turns, reflected};
}

} // namespace apex::geometry
