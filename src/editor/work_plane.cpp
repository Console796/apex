#include "editor/work_plane.hpp"

#include "geometry/solid_geometry.hpp"
#include "geometry/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace apex::editor {
namespace {

SurfacePlane frame(DVec3 normal, DVec3 origin) {
    const auto magnitude = glm::abs(normal);
    DVec3 u{1.0, 0.0, 0.0}, v{0.0, 1.0, 0.0};
    if (magnitude.z >= magnitude.x && magnitude.z >= magnitude.y) {
        // Dominant Z: X/Y.
    } else if (magnitude.x >= magnitude.y) {
        u = {0.0, 1.0, 0.0};
        v = {0.0, 0.0, 1.0};
    } else {
        v = {0.0, 0.0, 1.0};
    }
    // Project the two non-dominant world axes, then Gram-Schmidt v against u.
    // Keep u's world-positive direction and flip only v for outward handedness.
    u = glm::normalize(u - normal * glm::dot(normal, u));
    v -= normal * glm::dot(normal, v);
    v = glm::normalize(v - u * glm::dot(u, v));
    if (glm::dot(glm::cross(u, v), normal) < 0.0) v = -v;
    return {origin, normal, u, v, std::nullopt};
}

} // namespace

double GridLadder::clamp(double value) {
    if (!std::isfinite(value) || value <= 0.0) return kDefault;
    if (value <= kMin) return kMin;
    if (value >= kMax) return kMax;
    const double lower = std::exp2(std::floor(std::log2(value)));
    return value - lower < lower * 2.0 - value ? lower : lower * 2.0;
}

bool GridLadder::isValid(double value) {
    if (!std::isfinite(value) || value < kMin || value > kMax) return false;
    int exponent;
    return std::frexp(value, &exponent) == 0.5;
}

double GridLadder::larger(double value) { return std::min(clamp(value) * 2.0, kMax); }
double GridLadder::smaller(double value) { return std::max(clamp(value) * 0.5, kMin); }

std::optional<SurfacePlane> planeFromFace(const geometry::ConvexPolyhedron& poly,
                                         const geometry::Face& face) {
    const auto plane = poly.derivePlane(face);
    if (!plane) return std::nullopt;
    // Area relative to a face vertex: avoid products of large world coordinates near +/-65536.
    const auto anchor = poly.vertices[face.vertices.front()];
    double area = 0.0;
    for (std::size_t i = 1; i + 1 < face.vertices.size(); ++i) {
        const auto a = poly.vertices[face.vertices[i]] - anchor;
        const auto b = poly.vertices[face.vertices[i + 1]] - anchor;
        area += glm::dot(glm::cross(a, b), plane->normal);
    }
    if (!std::isfinite(area) || area <= 2.0 * geometry::kMinFaceArea) return std::nullopt;
    // Anchor at the world origin's projection (D9): u/v are world axes on axis-aligned faces, so the
    // absolute grid lines there are the world grid's.
    auto result = frame(plane->normal, plane->normal * plane->dist);
    Rect2D bounds{DVec2{std::numeric_limits<double>::infinity()},
                  DVec2{-std::numeric_limits<double>::infinity()}};
    for (const auto index : face.vertices) {
        const auto relative = poly.vertices[index] - result.origin;
        const DVec2 point{glm::dot(relative, result.uAxis), glm::dot(relative, result.vAxis)};
        bounds.min = glm::min(bounds.min, point);
        bounds.max = glm::max(bounds.max, point);
    }
    result.bounds = bounds;
    return result;
}

std::optional<SurfacePlane> planeFromFace(const document::Document& doc, NodeId node, SideId side) {
    const auto handle = doc.handle(node);
    if (!handle || !std::holds_alternative<Handle<document::Solid>>(*handle)) return std::nullopt;
    const auto geometry = geometry::solidGeometry(*doc.get(*handle)->block);
    if (!geometry) return std::nullopt;
    for (const auto& face : geometry->poly.faces) {
        if (static_cast<SideId>(face.sideId) == side) return planeFromFace(geometry->poly, face);
    }
    return std::nullopt;
}

void WorkPlaneState::setHovered(std::optional<SurfacePlane> plane) { hovered_ = std::move(plane); }
void WorkPlaneState::pinHovered() { if (hovered_) pinned_ = hovered_; }
void WorkPlaneState::pin(const SurfacePlane& plane) { pinned_ = plane; }
void WorkPlaneState::pinAxisPlane(Axis axis, DVec3 through) {
    DVec3 normal{0.0};
    normal[axis == Axis::X ? 0 : axis == Axis::Y ? 1 : 2] = 1.0;
    pinned_ = frame(normal, through);
}
void WorkPlaneState::unpin() { pinned_.reset(); }

std::optional<WorkPlaneState::Resolved> WorkPlaneState::resolve() const {
    if (pinned_) return Resolved{*pinned_, WorkPlaneSource::Pinned};
    if (hovered_) return Resolved{*hovered_, WorkPlaneSource::Hovered};
    return std::nullopt;
}

} // namespace apex::editor
