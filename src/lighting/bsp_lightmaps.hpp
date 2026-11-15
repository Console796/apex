#pragma once

#include "lighting/bake_scene.hpp"

#include <filesystem>
#include <array>
#include <map>
#include <optional>
#include <span>

namespace apex::lighting {

// One face of a compiled BSP with its style-0 HDR luxels (FACES_HDR + LIGHTING_HDR). vbsp splits and
// CSG-cuts a VMF side into many of these. `hammerId` is the FACEIDS lump's side id; P2CE's vbsp leaves that
// lump empty, so it is usually 0 and the fragment is matched to its side by plane and containment instead.
struct BspFragment {
    std::uint16_t hammerId = 0; // 0 when the lump has none
    bool displacement = false;
    std::vector<DVec3> vertices; // As stored: clockwise about `plane`
    Plane plane;                 // The lit side
    std::array<DVec4, 2> lightmapVectors{}; // texinfo s/t, xyz = direction per luxel, w = offset
    std::array<int, 2> mins{}, extents{};   // Stored lightmap origin and size in luxels
    std::vector<Vec3> luxels;    // (extents + 1) per axis, row-major; empty when the face has no data
};

// Reads every renderable (non-sky, non-NOLIGHT) face of a v25 BSP. Fails when the HDR lump is missing
// or the lumps disagree, never falls back to LDR.
Result<std::vector<BspFragment>> readBspFragments(const std::filesystem::path& bsp);

// Document sides, with the geometry a fragment is matched against. `loop` is counter-clockwise about
// `plane.normal` (outward).
struct SideOwner {
    std::uint64_t solidId = 0;
    std::uint32_t sideId = 0;
    Plane plane;
    std::vector<DVec3> loop;
    // A displaced side (plan 0.8.5): its D2 mesh and base quad in Source grid order (displacementGridCorners).
    // Only displacement fragments map to it, and only they carry these.
    std::optional<geometry::DisplacementMesh> displacement;
    std::array<DVec3, 4> gridCorners{};
};
using SideOwners = std::vector<SideOwner>;
// Every side of every solid in the document that has a face in game (instances must already be expanded):
// all sides of a brush without displacements, and the displaced sides (with their mesh) of one with them.
SideOwners sideOwners(const document::Document& document);

struct BspMapStats {
    std::size_t fragments = 0, mapped = 0;
    std::size_t displacement = 0, noData = 0, unowned = 0, mismatched = 0;
    std::size_t shared = 0; // Mapped fragments that cover several sides (merged faces, or coplanar overlap)
    std::size_t faces = 0; // Distinct (solid, side) pairs with at least one mapped fragment
};

// The compiled lightmaps in the layout LightmapAtlas takes. faces[i] has the exact VRAD lattice
// (luxel (x, y) is the sample at stored mins + (x, y)); radiance[i] holds its luxels. Fragments that
// carry no light or lie on no side's plane and polygon are left out, so the viewport shows them black.
// A flat fragment belongs to the side whose plane it lies on (normal within float noise, distance within
// 0.25) and whose polygon contains all its vertices; when the FACEIDS lump did name a side, only that side
// is considered. Several such sides is ambiguous. A displacement fragment (its BSP face is the base quad)
// belongs to the displaced side whose plane it lies on and whose four corners it matches within 0.25; it
// becomes that side's displaced BakeFace with the fragment's chart and a lattice for the fragment's extents
// (P0: luxel (s, t) at grid fraction (t / extents[1], s / extents[0])). Flat fragments never map to
// displaced sides, nor the reverse.
struct BspLightmaps {
    std::vector<BakeFace> faces;
    std::vector<std::vector<Vec3>> radiance;
    BspMapStats stats;
};
BspLightmaps mapBspFragments(std::span<const BspFragment> fragments, const SideOwners& owners);

} // namespace apex::lighting
