#pragma once

#include "lighting/bake_scene.hpp"

#include <filesystem>
#include <map>
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
};
using SideOwners = std::vector<SideOwner>;
// Every non-displacement side of every solid in the document (instances must already be expanded).
SideOwners sideOwners(const document::Document& document);

struct BspMapStats {
    std::size_t fragments = 0, mapped = 0;
    std::size_t displacement = 0, noData = 0, unowned = 0, mismatched = 0;
    std::size_t shared = 0; // Mapped fragments that cover several sides (merged faces, or coplanar overlap)
    std::size_t faces = 0; // Distinct (solid, side) pairs with at least one mapped fragment
};

// The compiled lightmaps in the layout LightmapAtlas takes. faces[i] has the exact VRAD lattice
// (luxel (x, y) is the sample at stored mins + (x, y)); radiance[i] holds its luxels. Fragments that
// are displacements, carry no light, or lie on no side's plane and polygon
// are left out, so the viewport shows them black. A fragment belongs to the side whose plane it lies on
// (normal within float noise, distance within 0.25) and whose polygon contains all its vertices; when
// the FACEIDS lump did name a side, only that side is considered. Several such sides is ambiguous.
struct BspLightmaps {
    std::vector<BakeFace> faces;
    std::vector<std::vector<Vec3>> radiance;
    BspMapStats stats;
};
BspLightmaps mapBspFragments(std::span<const BspFragment> fragments, const SideOwners& owners);

} // namespace apex::lighting
