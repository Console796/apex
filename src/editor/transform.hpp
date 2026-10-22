#pragma once

#include "editor/work_plane.hpp"

#include <map>
#include <span>

namespace apex::editor {

class Transform {
public:
    Transform() = default;
    // Finite, invertible affine maps only; invalid inputs throw invalid_argument.
    Transform(DMat3 linear, DVec3 translation);
    static Transform translate(DVec3 delta);
    static Transform rotate(DVec3 axis, double angleDeg, DVec3 pivot = DVec3{0.0});
    // Frame columns are orthonormal axes in world coordinates.
    static Transform scale(DVec3 factors, DMat3 frame = DMat3{1.0}, DVec3 pivot = DVec3{0.0});
    static Transform mirror(DVec3 normal, DVec3 pivot = DVec3{0.0});

    DVec3 point(DVec3 value) const { return linear_ * value + translation_; }
    const DMat3& linear() const { return linear_; }
    DVec3 translation() const { return translation_; }
    DMat4 matrix() const;
    Transform inverse() const;
    bool isRigid() const; // Includes reflections (orthogonal linear part).
    bool reversesOrientation() const;
    // (a * b)(p) = a(b(p)).
    Transform operator*(const Transform& other) const;

private:
    DMat3 linear_{1.0};
    DVec3 translation_{0.0};
};

enum class SnapMode { Absolute, Relative };
struct SnapSettings {
    SnapMode mode = SnapMode::Absolute;
    double gridSize = GridLadder::kDefault;
    double angleSnap = 15.0; // Degrees; fine uses one degree.
    bool fine = false;
    bool suspended = false;
};

// Bounds/grab are world coordinates. Snap in-plane only; retain the normal delta.
DVec3 snapTranslate(DVec3 rawDelta, const AABB& bounds, DVec3 grab,
                    const SurfacePlane& plane, const SnapSettings& settings);
// Returns a delta in degrees; startingAngle is measured in the gizmo frame.
double snapRotate(double rawDelta, double startingAngle, const SnapSettings& settings);
// Bounds and pivot are in the scale frame. Snap the endpoint farthest from the pivot
// on each active axis (ties prefer max); an unchanged factor leaves that axis alone.
DVec3 snapScale(DVec3 rawFactors, const AABB& bounds, DVec3 pivot,
                const SnapSettings& settings);

std::vector<DVec3> transformedVertices(const geometry::SolidGeometry& solid, const Transform& transform);
Result<geometry::SolidGeometry> applyToSolid(const geometry::SolidGeometry& solid,
                                            const Transform& transform, bool textureLock = true);

struct EntityTransform {
    std::optional<DVec3> origin, angles;
    std::map<NodeId, geometry::SolidGeometry> solids;
};
Result<EntityTransform> applyToEntity(const document::Document& doc, NodeId entity,
                                       const Transform& transform, bool textureLock = true);
struct TransformOptions {
    bool textureLock = true;
};
// The transform is already snapped by the drag/modal caller, shared with preview.
// Expand entity/group descendants and deduplicate overlapping selection nodes.
Result<void> commitTransform(document::Document& doc, std::span<const NodeId> selection,
                              const Transform& transform, TransformOptions options,
                              std::string label);

DVec3 nudgeDelta(const SurfacePlane& plane, bool alongV, double gridSize, int direction = 1);
// The caller supplies the viewport fallback when no hovered/pinned plane exists.
DVec3 nudgeDelta(const WorkPlaneState& state, const SurfacePlane& fallback, bool alongV,
                 double gridSize, int direction = 1);

} // namespace apex::editor
