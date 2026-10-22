#pragma once

#include "core/math.hpp"
#include "document/document.hpp"

#include <optional>

namespace apex::editor {

using document::NodeId;
using document::SideId;

struct GridLadder {
    static constexpr double kMin = 0.125;
    static constexpr double kMax = 1024.0;
    static constexpr double kDefault = 64.0;

    static double larger(double value);
    static double smaller(double value);
    static bool isValid(double value);
    // Nearest in world units; midpoint ties go to the larger entry.
    static double clamp(double value);
};

struct Rect2D {
    DVec2 min, max;
};

struct SurfacePlane {
    DVec3 origin, normal, uAxis, vAxis;
    std::optional<Rect2D> bounds; // Coordinates relative to origin in the u/v frame.
};

// Ties for the dominant normal component prefer Z, then X, then Y.
// Invalid solids, missing/discarded sides and degenerate faces return nullopt.
std::optional<SurfacePlane> planeFromFace(const document::Document& doc, NodeId node, SideId side);
// Reuse geometry already obtained through solidGeometry (e.g. when visiting many faces).
std::optional<SurfacePlane> planeFromFace(const geometry::ConvexPolyhedron& poly,
                                         const geometry::Face& face);

enum class Axis { X, Y, Z };
enum class WorkPlaneSource { Pinned, Hovered, Fallback };

class WorkPlaneState {
public:
    void setHovered(std::optional<SurfacePlane> plane);
    void pinHovered();
    void pinAxisPlane(Axis axis, DVec3 through);
    void unpin();
    bool pinned() const { return pinned_.has_value(); }

    struct Resolved {
        SurfacePlane plane;
        WorkPlaneSource source;
    };
    // No plane means the viewport supplies its perspective/axis-view fallback.
    std::optional<Resolved> resolve() const;

private:
    std::optional<SurfacePlane> hovered_, pinned_;
};

} // namespace apex::editor
