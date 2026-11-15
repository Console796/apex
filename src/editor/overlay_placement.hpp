#pragma once

// Plan 0.8 D12: the Overlay tool's pure parts. Placing a quad on a face, choosing the faces its
// footprint covers, and the handle drags that move, rotate and scale it or move a UV corner. The
// tool (overlay_tool.hpp) only turns pointer input into these calls and writes the result.

#include "document/overlay_fragments.hpp"
#include "editor/transform.hpp"
#include "geometry/overlay.hpp"

#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::editor {

// A face takes an overlay's footprint when its normal is within this angle of the basis normal.
inline constexpr double kOverlayFootprintDegrees = 45.0;
// A face whose share of the quad is smaller than this (square units) is not added.
inline constexpr double kOverlayFootprintMinArea = 1e-3;
// World units per texture pixel when the clicked face has no usable texture scale.
inline constexpr double kOverlayDefaultScale = 0.25;

// The quad an overlay placed at `hit` on `face` starts with: centred there, U along the face's
// texture U axis, sized as the material's texture would be on that face (pixels times the face's
// texture scale), so its aspect is the material's.
geometry::OverlayQuad placementQuad(const document::OverlaySurface& face, DVec3 hit, std::pair<int, int> textureSize);

// Every face the quad lands on (area above kOverlayFootprintMinArea) whose normal is within
// kOverlayFootprintDegrees of the basis normal, in the order of `surfaces`. Faces whose projection
// along the basis normal misses the quad's bounds are rejected before clipping.
std::vector<int> footprintSides(std::span<const document::OverlaySurface> surfaces, const geometry::OverlayQuad& quad);

// What document::overlayGeometry builds for an info_overlay with this quad, material and side list,
// resolving each side through `surface` (nullptr: missing) instead of the document. The tool's
// drag preview, so a drag re-clips only the overlay it edits against faces it read once.
document::OverlayGeometry quadGeometry(const geometry::OverlayQuad& quad, const std::string& material,
                                       std::span<const int> sides,
                                       const std::function<const document::OverlaySurface*(int side)>& surface);

// `sides` with `side` added, or removed if it was there; the order of the rest is kept.
std::vector<int> toggleSide(std::vector<int> sides, int side);

// The entity block of a new info_overlay or infodecal.
kv1::Node overlayBlock(const geometry::OverlayQuad& quad, const std::string& material, std::span<const int> sides);
kv1::Node decalBlock(DVec3 origin, const std::string& material);
// The keys an edited quad writes (basis, uv points, range and "origin", which follows BasisOrigin).
std::vector<std::pair<std::string, std::string>> quadKeys(const geometry::OverlayQuad& quad);

enum class OverlayHandleKind { Move, Rotate, ScaleU, ScaleV, Corner };
struct OverlayHandle {
    OverlayHandleKind kind = OverlayHandleKind::Move;
    int corner = 0; // Corner only: uv0..uv3.
    bool operator==(const OverlayHandle&) const = default;
};
struct OverlayHandleSpot {
    OverlayHandle handle;
    DVec3 position;
};
// Where each handle sits: the centre (move), a rotate knob past the +U edge, the midpoints of the
// +U and +V edges (scale), and the four corners.
std::vector<OverlayHandleSpot> overlayHandles(const geometry::OverlayQuad& quad);

// The quad after dragging `handle` of `start` from `from` to `to`, both points on the quad's plane.
// Move translates in the plane; Rotate turns U and V about the normal by the angle the cursor
// swept around the centre, in `snap`'s angle steps from the start like the gizmo (none without it);
// ScaleU and ScaleV scale the uv points along one axis about the centre (never through zero); Corner
// moves that one uv point.
geometry::OverlayQuad dragOverlayHandle(const geometry::OverlayQuad& start, OverlayHandle handle, DVec3 from, DVec3 to,
                                        const SnapSettings* snap = nullptr);

} // namespace apex::editor
