#pragma once

#include "editor/pick_index.hpp"
#include "editor/transform.hpp"

#include <utility>

namespace apex::editor {

enum class SnapKind { Vertex, EdgeMidpoint, Edge, Grid, Surface, None };
struct SnapRequest {
    SnapRequest(bool suspend = false, std::optional<SurfacePlane> lockedPlane = {}, bool snapEdges = true)
        : suspended(suspend), plane(std::move(lockedPlane)), edges(snapEdges) {}
    bool suspended = false;
    // Locks grid/surface fallback to this plane; feature priority is unchanged.
    std::optional<SurfacePlane> plane;
    bool edges = true; // Includes closest points and midpoints.
};
struct SnapResult {
    DVec3 point{0.0};
    SnapKind kind = SnapKind::None;
    std::optional<NodeId> node;
    std::optional<SideId> side;
};

class Snapper {
public:
    Snapper(const document::Document& document, const PickIndex& picks,
            const WorkPlaneState& workPlane, SnapSettings settings, SurfacePlane fallback);
    SnapResult snap(Ray ray, const ToolView& view, const SnapRequest& request = {}) const;

private:
    const document::Document& document_;
    const PickIndex& picks_;
    const WorkPlaneState& workPlane_;
    SnapSettings settings_;
    SurfacePlane fallback_;
};

} // namespace apex::editor
