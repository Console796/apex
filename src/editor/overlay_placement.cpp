#include "editor/overlay_placement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace apex::editor {
namespace {

// Smallest half-extent a scale drag leaves along an axis.
constexpr double kMinHalfExtent = 0.5;

DVec2 inPlane(const geometry::OverlayQuad& quad, DVec3 point) {
    const DVec3 d = point - quad.origin;
    return {glm::dot(d, quad.basisU), glm::dot(d, quad.basisV)};
}

// Whether `surface` projected along the quad's normal (as the clip projects it) lies outside the
// quad's bounds in its plane; `toLocal` maps world offsets from the origin to (u, v, along normal).
bool outsideQuad(const document::OverlaySurface& surface, const geometry::OverlayQuad& quad, const DMat3& toLocal,
                 DVec2 low, DVec2 high) {
    DVec2 min{std::numeric_limits<double>::infinity()}, max{-std::numeric_limits<double>::infinity()};
    const auto add = [&](DVec3 p) {
        const DVec3 local = toLocal * (p - quad.origin);
        min = glm::min(min, DVec2(local));
        max = glm::max(max, DVec2(local));
    };
    if (surface.displacement) for (const auto& p : surface.displacement->positions) add(p);
    else for (const auto& p : surface.polygon) add(p);
    constexpr double kSlack = 1e-6;
    return max.x < low.x - kSlack || min.x > high.x + kSlack || max.y < low.y - kSlack || min.y > high.y + kSlack;
}

double extent(const geometry::OverlayQuad& quad, int axis) {
    double result = 0.0;
    for (const auto& p : quad.points) result = std::max(result, axis == 0 ? p.x : p.y);
    return result;
}

} // namespace

geometry::OverlayQuad placementQuad(const document::OverlaySurface& face, DVec3 hit, std::pair<int, int> textureSize) {
    const auto scale = [](double s) { return std::isfinite(s) && std::abs(s) > 1e-6 ? std::abs(s) : kOverlayDefaultScale; };
    const DVec2 size{std::max(textureSize.first, 1) * scale(face.uv.scaleU),
                     std::max(textureSize.second, 1) * scale(face.uv.scaleV)};
    return geometry::faceAlignedQuad(hit, face.normal, face.uv.uaxis, size);
}

std::vector<int> footprintSides(std::span<const document::OverlaySurface> surfaces, const geometry::OverlayQuad& quad) {
    const double minCosine = std::cos(glm::radians(kOverlayFootprintDegrees)) - 1e-9;
    // OverlayQuad::local's basis; a degenerate one clips nothing, so it is left to the clip.
    const DMat3 basis(quad.basisU, quad.basisV, quad.normal);
    const bool cull = std::abs(glm::determinant(basis)) > 1e-12;
    const DMat3 toLocal = cull ? glm::inverse(basis) : DMat3(1.0);
    DVec2 low{std::numeric_limits<double>::infinity()}, high{-std::numeric_limits<double>::infinity()};
    for (const auto& p : quad.points) {
        low = glm::min(low, p);
        high = glm::max(high, p);
    }
    std::vector<int> sides;
    for (const auto& surface : surfaces) {
        if (glm::dot(surface.normal, quad.normal) < minCosine) continue;
        if (cull && outsideQuad(surface, quad, toLocal, low, high)) continue;
        const auto fragment = document::clipToSurface(quad, surface);
        if (!fragment || geometry::fragmentArea(*fragment) <= kOverlayFootprintMinArea) continue;
        const int side = static_cast<int>(surface.side);
        if (std::ranges::find(sides, side) == sides.end()) sides.push_back(side);
    }
    return sides;
}

document::OverlayGeometry quadGeometry(const geometry::OverlayQuad& quad, const std::string& material,
                                       std::span<const int> sides,
                                       const std::function<const document::OverlaySurface*(int side)>& surface) {
    document::OverlayGeometry result;
    result.kind = document::OverlayKind::Overlay;
    result.material = material;
    result.quads.push_back(quad);
    for (const int side : sides) {
        result.listed.push_back(static_cast<document::SideId>(side));
        const auto* face = surface(side);
        if (!face) {
            result.missing.push_back(static_cast<document::SideId>(side));
            continue;
        }
        if (auto fragment = document::clipToSurface(quad, *face)) result.pieces.push_back({std::move(*fragment), face->solid});
    }
    return result;
}

std::vector<int> toggleSide(std::vector<int> sides, int side) {
    if (const auto found = std::ranges::find(sides, side); found != sides.end()) sides.erase(found);
    else sides.push_back(side);
    return sides;
}

std::vector<std::pair<std::string, std::string>> quadKeys(const geometry::OverlayQuad& quad) {
    auto keys = geometry::overlayKeys(quad);
    keys.emplace_back("origin", keys.front().second); // BasisOrigin, formatted the same way.
    return keys;
}

kv1::Node overlayBlock(const geometry::OverlayQuad& quad, const std::string& material, std::span<const int> sides) {
    auto block = kv1::Node::block("entity");
    const auto add = [&](std::string key, std::string value) {
        block.append(kv1::Node::value(std::move(key), std::move(value))).value();
    };
    add("classname", "info_overlay");
    add("material", material);
    add("sides", geometry::sideListText(sides));
    for (auto& [key, value] : quadKeys(quad)) add(key, value);
    return block;
}

kv1::Node decalBlock(DVec3 origin, const std::string& material) {
    geometry::OverlayQuad at;
    at.origin = origin;
    auto block = kv1::Node::block("entity");
    block.append(kv1::Node::value("classname", "infodecal")).value();
    block.append(kv1::Node::value("texture", material)).value();
    block.append(kv1::Node::value("origin", geometry::overlayKeys(at).front().second)).value();
    return block;
}

std::vector<OverlayHandleSpot> overlayHandles(const geometry::OverlayQuad& quad) {
    const double u = extent(quad, 0), v = extent(quad, 1);
    std::vector<OverlayHandleSpot> spots{
        {{OverlayHandleKind::Move}, quad.origin},
        {{OverlayHandleKind::Rotate}, quad.origin + quad.basisU * (u + std::max(8.0, u * 0.25))},
        {{OverlayHandleKind::ScaleU}, quad.origin + quad.basisU * u},
        {{OverlayHandleKind::ScaleV}, quad.origin + quad.basisV * v},
    };
    for (int i = 0; i < 4; ++i) spots.push_back({{OverlayHandleKind::Corner, i}, quad.corner(static_cast<std::size_t>(i))});
    return spots;
}

geometry::OverlayQuad dragOverlayHandle(const geometry::OverlayQuad& start, OverlayHandle handle, DVec3 from, DVec3 to) {
    auto quad = start;
    const DVec2 a = inPlane(start, from), b = inPlane(start, to);
    switch (handle.kind) {
    case OverlayHandleKind::Move:
        quad.origin = start.origin + start.basisU * (b.x - a.x) + start.basisV * (b.y - a.y);
        break;
    case OverlayHandleKind::Rotate: {
        if (glm::length(a) < 1e-9 || glm::length(b) < 1e-9) break;
        const double angle = std::atan2(b.y, b.x) - std::atan2(a.y, a.x);
        const double c = std::cos(angle), s = std::sin(angle);
        quad.basisU = glm::normalize(start.basisU * c + start.basisV * s);
        quad.basisV = glm::normalize(start.basisV * c - start.basisU * s);
        break;
    }
    case OverlayHandleKind::ScaleU:
    case OverlayHandleKind::ScaleV: {
        const int axis = handle.kind == OverlayHandleKind::ScaleU ? 0 : 1;
        const double half = extent(start, axis);
        if (!(half > 0.0)) break;
        const double grabbed = std::max(a[axis], 1e-9);
        const double factor = std::max(b[axis], kMinHalfExtent) / grabbed;
        for (auto& p : quad.points) p[axis] *= factor;
        break;
    }
    case OverlayHandleKind::Corner:
        quad.points[static_cast<std::size_t>(handle.corner & 3)] += b - a;
        break;
    }
    return quad;
}

} // namespace apex::editor
