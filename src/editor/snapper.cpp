#include "editor/snapper.hpp"

#include <cmath>

namespace apex::editor {

Snapper::Snapper(const document::Document& document, const PickIndex& picks,
                 const WorkPlaneState& workPlane, SnapSettings settings, SurfacePlane fallback)
    : document_(document), picks_(picks), workPlane_(workPlane), settings_(settings), fallback_(fallback) {}

SnapResult Snapper::snap(Ray ray, const ToolView& view, const SnapRequest& request) const {
    const bool suspended = request.suspended || settings_.suspended;
    const auto hit = picks_.first(ray);
    if (!suspended) {
        const auto features = picks_.nearestFeature(ray, 8.0, view);
        const auto result = [](const FeatureHit& feature, SnapKind kind) {
            return SnapResult{feature.point, kind, feature.node, feature.side};
        };
        if (features.vertex) return result(*features.vertex, SnapKind::Vertex);
        if (request.edges && features.midpoint) return result(*features.midpoint, SnapKind::EdgeMidpoint);
        if (request.edges && features.edge) return result(*features.edge, SnapKind::Edge);
    }
    if (suspended && hit && !request.plane) return {hit->point, SnapKind::Surface, hit->node, hit->side};

    auto resolved = workPlane_.resolve();
    SurfacePlane plane = request.plane ? *request.plane : resolved ? resolved->plane : fallback_;
    std::optional<NodeId> node;
    std::optional<SideId> side;
    // A pin owns the drawing plane; otherwise use this ray's surface rather than stale hover.
    if (!request.plane && !workPlane_.pinned() && hit && hit->side) {
        if (const auto surface = planeFromFace(document_, hit->node, *hit->side)) {
            plane = *surface;
            node = hit->node;
            side = hit->side;
        }
    }
    const double denominator = glm::dot(plane.normal, ray.dir);
    if (std::abs(denominator) > 1e-12) {
        const double t = glm::dot(plane.origin - ray.origin, plane.normal) / denominator;
        if (t >= 0.0 && std::isfinite(t)) {
            const auto raw = ray.at(t);
            if (suspended) return {raw, SnapKind::Surface, node, side};
            auto settings = settings_;
            settings.mode = SnapMode::Absolute;
            settings.gridSize = GridLadder::clamp(settings.gridSize);
            const auto snapped = raw + snapTranslate(DVec3{0.0}, AABB{raw, raw}, raw, plane, settings);
            return {snapped, SnapKind::Grid, node, side};
        }
    }
    if (hit && !request.plane) return {hit->point, SnapKind::Surface, hit->node, hit->side};
    return {};
}

} // namespace apex::editor
