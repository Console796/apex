#pragma once

#include "document/document.hpp"
#include "geometry/overlay.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::document {

// Plan 0.8 D10: what an overlay-like entity draws, built from the document on demand.
enum class OverlayKind { Overlay, Transition, Decal };
std::optional<OverlayKind> overlayKind(std::string_view classname);
std::optional<OverlayKind> overlayKind(const kv1::Node& entity);

// One face an overlay can land on, in world space: its convex polygon, or its displaced mesh.
struct OverlaySurface {
    SideId side = 0;
    NodeId solid = 0;
    std::vector<DVec3> polygon; // Counter-clockwise from outside (the base face of a displacement).
    DVec3 normal{0.0, 0.0, 1.0};
    std::optional<geometry::DisplacementMesh> displacement;
    std::string material;
    geometry::FaceUv uv;
};

// Resolves sides to surfaces, parsing each solid once. Returns nullopt for an unknown side or a
// solid that does not form a valid polyhedron. A displacement that does not parse falls back to
// the base face.
class SurfaceLookup {
public:
    explicit SurfaceLookup(const Document& doc) : doc_(doc) {}
    std::optional<OverlaySurface> surface(SideId side);
    // Every face of every solid in the document (for the decal search and the Overlay tool).
    std::vector<OverlaySurface> all();

private:
    const geometry::SolidGeometry* solid(NodeId id, const kv1::Node& block);
    OverlaySurface make(NodeId solid, const geometry::SolidGeometry& shape, const geometry::Face& face);
    const Document& doc_;
    std::map<NodeId, std::optional<geometry::SolidGeometry>> solids_;
};

// Clips one quad onto one surface: the displaced mesh if there is one, else the polygon.
std::optional<geometry::OverlayFragment> clipToSurface(const geometry::OverlayQuad& quad, const OverlaySurface& surface);

struct OverlayPiece {
    geometry::OverlayFragment fragment;
    NodeId solid = 0;
};
struct OverlayGeometry {
    OverlayKind kind = OverlayKind::Overlay;
    std::string material;
    std::vector<geometry::OverlayQuad> quads; // What was projected (one, or one per transition segment).
    std::vector<OverlayPiece> pieces;         // In side-list order.
    std::vector<SideId> listed;               // The entity's side list (sides, then sides2).
    std::vector<SideId> missing;              // Listed sides the document does not have.
    bool approximate = false;                 // infodecal: placed by the engine at load, not by VBSP.
    std::optional<std::string> problem;       // Why the entity could not be parsed.
};

// A decal's size in world units for its material (texture pixels times $decalscale).
using DecalSize = std::function<DVec2(std::string_view material)>;

// The infodecal approximation (D10, Bite 9): the nearest face whose plane is within half the
// decal's larger extent of its origin (tool textures excluded), then the decal square aligned to
// that face's texture U axis, clipped against every coplanar face it touches.
// `surfaces` may be every face of the map or any superset of those within reach (decalCandidates).
OverlayGeometry decalGeometry(std::span<const OverlaySurface> surfaces, const kv1::Node& entity, DVec2 size);
// The faces a decal of any size up to `reach` world units could land on, for recomputing it when
// its material's size becomes known without consulting the document again.
std::vector<OverlaySurface> decalCandidates(SurfaceLookup& surfaces, const kv1::Node& entity, double reach);

// The fragments of an info_overlay, info_overlay_transition or infodecal. Sides that fail to
// resolve are listed in `missing`; a malformed basis sets `problem` and yields no pieces.
OverlayGeometry overlayGeometry(SurfaceLookup& surfaces, const kv1::Node& entity, const DecalSize& decalSize = {});
OverlayGeometry overlayGeometry(const Document& doc, const kv1::Node& entity, const DecalSize& decalSize = {});

// The overlay-like entities that list `side`, by node ID (Face Properties' "Overlays on this face").
std::vector<NodeId> overlaysOnSide(const Document& doc, SideId side);

// Every entity node of the document, depth first, including those inside hidden containers.
std::vector<NodeId> entityNodes(const Document& doc);

} // namespace apex::document
