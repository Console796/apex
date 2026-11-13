#pragma once

#include "core/kv1.hpp"
#include "core/math.hpp"
#include "core/result.hpp"
#include "geometry/displacement.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::geometry {

// Plan 0.8 D10. An info_overlay is a quad in its own basis: uv0..uv3 are (u, v) coordinates along
// BasisU and BasisV from BasisOrigin (their third component is ignored, as by VBSP), projected
// along BasisNormal onto each listed face. The engine, not VBSP, clips the quad at map load
// (VBSP only maps side IDs to compiled faces); Apex clips the same way against the brush faces.
//
// Texture coordinates follow the engine: corner 0 is (StartU, StartV), corner 1 (StartU, EndV),
// corner 2 (EndU, EndV) and corner 3 (EndU, StartV); points inside are mapped by inverse bilinear
// interpolation of the four corners.
struct OverlayQuad {
    DVec3 origin{0.0};
    DVec3 basisU{1.0, 0.0, 0.0};
    DVec3 basisV{0.0, 1.0, 0.0};
    DVec3 normal{0.0, 0.0, 1.0};
    std::array<DVec2, 4> points{DVec2{-16, -16}, DVec2{-16, 16}, DVec2{16, 16}, DVec2{16, -16}};
    double startU = 0.0, endU = 1.0, startV = 0.0, endV = 1.0;

    DVec3 corner(std::size_t index) const { return origin + basisU * points[index].x + basisV * points[index].y; }
    std::array<DVec2, 4> cornerTexCoords() const {
        return {DVec2{startU, startV}, DVec2{startU, endV}, DVec2{endU, endV}, DVec2{endU, startV}};
    }
    // (u, v) of a world point, along the basis normal; nullopt for a singular basis.
    std::optional<DVec2> local(DVec3 world) const;
    // Inverse bilinear texture coordinate of a local point (extrapolated outside the quad).
    DVec2 texCoord(DVec2 local) const;
    // The first problem (nonfinite value, degenerate basis or quad), or nullopt.
    std::optional<std::string> problem() const;
    bool operator==(const OverlayQuad&) const = default;
};

// Reads BasisOrigin, BasisU, BasisV, BasisNormal, uv0..uv3 and StartU/EndU/StartV/EndV. A missing
// BasisV is BasisNormal x BasisU, a missing range 0..1. Malformed or degenerate values are errors.
Result<OverlayQuad> parseOverlay(const kv1::Node& entity);
// The keys parseOverlay reads, as Hammer spells them, with shortest round-trip numbers.
std::vector<std::pair<std::string, std::string>> overlayKeys(const OverlayQuad& quad);
// Space-separated side IDs of a sidelist value; words that are not integers are skipped.
std::vector<int> parseSideList(std::string_view text);
std::string sideListText(std::span<const int> sides);

struct OverlayVertex {
    DVec3 position{0.0};
    DVec2 uv{0.0};
};
// One clipped piece on one face: a convex polygon on a flat face, or triangles on a displacement.
struct OverlayFragment {
    std::uint32_t side = 0;
    DVec3 normal{0.0, 0.0, 1.0}; // The surface's outward normal (for shading).
    std::vector<OverlayVertex> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

// Faces whose normal is within this cosine of perpendicular to the projection direction receive
// nothing (the projection is too oblique to be meaningful). A face facing away from BasisNormal
// still receives the overlay, as in the engine: shipped maps list such faces (the BSP oracle has
// three) and VBSP keeps them.
inline constexpr double kOverlayMinCosine = 1e-3;

// Clips the quad against a flat convex polygon (counter-clockwise from outside, with its outward
// normal). Points are projected along quad.normal onto the polygon's plane. Empty when nothing of
// the quad lands on the polygon or the face is edge-on or facing away.
std::optional<OverlayFragment> clipOverlay(const OverlayQuad& quad, std::span<const DVec3> polygon, DVec3 faceNormal,
                                           std::uint32_t side = 0);
// Clips against every triangle of a displacement mesh, each lifted onto its own plane.
std::optional<OverlayFragment> clipOverlay(const OverlayQuad& quad, const DisplacementMesh& mesh, std::uint32_t side = 0);

// Area of a fragment (sum of its triangles).
double fragmentArea(const OverlayFragment& fragment);
// Area of the quad in its own plane.
double quadArea(const OverlayQuad& quad);

// D12 and the infodecal approximation: a quad of `size` world units centred on `centre`, lying on a
// face with outward `normal`, with U along the face's texture U axis projected into the plane
// (or any perpendicular axis when the projection vanishes) and V = normal x U, so the texture's
// top is up as it is on the face. Texture coordinates run 0..1 left to right and top to bottom.
OverlayQuad faceAlignedQuad(DVec3 centre, DVec3 normal, DVec3 textureU, DVec2 size);

// info_overlay_transition (D10): the shoreline strip where the land faces meet the water plane.
// Each land polygon's intersection with the water plane gives a segment; the strip extends
// `landWidth` towards the land and `waterWidth` over the water, on the water plane, with U along
// the shore (lengthStart..lengthEnd per 128 units, as the texture repeats) and V across it
// (widthStart at the land edge, widthEnd at the water edge). One quad per land face.
struct TransitionShape {
    double landWidth = 25.0, waterWidth = 25.0;
    double lengthStart = 0.0, lengthEnd = 1.0;
    double widthStart = 0.0, widthEnd = 1.0;
};
TransitionShape parseTransition(const kv1::Node& entity);
std::vector<OverlayQuad> transitionQuads(const TransitionShape& shape, const Plane& water,
                                         std::span<const std::vector<DVec3>> landPolygons);

// Carry an overlay through p -> linear * p + translation (moves, rotations, scales and mirrors of
// the entity with its faces). The basis stays unit length; uv points scale with the map along
// each axis, so the quad's world corners are the mapped corners.
OverlayQuad transformOverlay(const OverlayQuad& quad, const DMat3& linear, DVec3 translation);

} // namespace apex::geometry
