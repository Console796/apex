#pragma once

#include "editor/transform.hpp"

#include <array>
#include <string>

namespace apex::editor {

enum class GizmoMode { Translate, Rotate, Scale };
enum class GizmoSpace { Global, Local };
enum class PivotMode { BoundsCentre, ActiveOrigin, Custom };
enum class GizmoHandle { X, Y, Z, XY, YZ, ZX, Screen, Uniform };

// Camera axes are optional: cameraUp also supports rolled and orthographic views.
// pixelsPerWorldUnit is evaluated by the caller at the pivot's camera distance.
struct GizmoView {
    DVec3 cameraPosition{0, 0, 1000};
    double pixelsPerWorldUnit = 1.0;
    double sizePixels = 80.0;
    DVec3 cameraUp{0, 1, 0};
};
struct GizmoContext {
    AABB bounds{DVec3{-1.0}, DVec3{1.0}};
    DVec3 pivot{0.0};
    DMat3 frame{1.0};
    DMat3 localFrame{1.0};
    DVec3 rotationStartDeg{0.0}; // About X/Y/Z, zero in the active local frame.
    SurfacePlane workPlane{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {}};
    SnapSettings snap;
};

enum class GizmoShape { Segment, Ring, Quad };
struct GizmoPrimitive {
    GizmoHandle handle;
    GizmoShape shape;
    std::array<DVec3, 4> points{}; // Segment: first two; quad: ordered corners.
    DVec3 centre{0.0}, normal{0, 0, 1};
    double radius = 0; // Ring (including the screen rotation ring).
};
std::vector<GizmoPrimitive> gizmoDescription(GizmoMode mode, const GizmoContext& context,
                                            const GizmoView& view);
struct GizmoHit {
    GizmoHandle handle;
    double rayParameter, distancePixels;
};
// Exact segment/quad geometry and stationary ray/circle distance; no tessellation.
// Smallest forward ray parameter wins; pixel distance resolves depth ties.
std::optional<GizmoHit> hitTestGizmo(GizmoMode mode, const GizmoContext& context,
                                    const GizmoView& view, Ray ray, double tolerancePixels = 6.0);

struct TransformModifiers { bool suspend = false, fine = false; };
struct TransformReadout {
    Transform transform;
    DVec3 delta{0.0}, factors{1.0};
    double angleDeg = 0;
    std::string text;
};
// Shared constraint + snapping path. raw is a world delta, angle in x, or frame scale factors.
// Exact input bypasses snapping. Invalid/singular values return nullopt.
std::optional<TransformReadout> evaluateTransform(GizmoMode mode, GizmoHandle handle,
    const GizmoContext& context, DVec3 grab, DVec3 raw, TransformModifiers modifiers = {},
    bool exact = false, DVec3 screenNormal = DVec3{0, 0, 1});

class GizmoDrag {
public:
    bool begin(GizmoMode mode, GizmoHandle handle, const GizmoContext& context,
               const GizmoView& view, Ray ray);
    const TransformReadout& update(Ray ray, TransformModifiers modifiers = {});
    const TransformReadout& value() const { return value_; }
    bool active() const { return active_; }
    void cancel() { active_ = false; }
private:
    GizmoMode mode_ = GizmoMode::Translate;
    GizmoHandle handle_ = GizmoHandle::X;
    GizmoContext context_;
    DVec3 normal_{0, 0, 1}, start_{0.0}, axis_{1, 0, 0};
    double previousAngle_ = 0, accumulatedAngle_ = 0;
    bool active_ = false;
    TransformReadout value_;
    std::optional<DVec3> position(Ray ray) const;
};

} // namespace apex::editor
