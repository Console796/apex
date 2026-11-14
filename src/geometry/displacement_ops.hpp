#pragma once

// Plan 0.8 D6: the Displacement tool's pure operations (Create, Sew, Subdivide) and the seam
// measure behind the "unsewn displacement edge" problem. Corners are always a base face's four
// corners in geometry::Face order, as displacementMesh takes them.

#include "geometry/displacement.hpp"

#include <array>
#include <span>
#include <vector>

namespace apex::geometry {

// Hammer's walkable and buildable slopes: a triangle whose normal has z at least this much gets
// the bit. kBuildableNormalZ is pinned by test1's tags; kWalkableNormalZ (the player's 45.57
// degree limit) only agrees with them, since every test1 triangle is walkable.
inline constexpr double kWalkableNormalZ = 0.7, kBuildableNormalZ = 0.8;

// The base corner Hammer starts a new displacement at: the corner nearest the minimum of the
// corners' bounding box (CCoreDispSurface's rule, from memory; test1 agrees). Ties go to the
// earlier corner in Face order.
DVec3 displacementStartCorner(std::span<const DVec3> corners);

// The base face's outward unit normal (Newell's method over the four corners).
Result<DVec3> displacementBaseNormal(std::span<const DVec3> corners);

// A new displacement as Hammer writes one: normals and offset normals are the base normal,
// distances, offsets and alphas are 0, subdiv 0, flags 0, every allowed_verts bit of the largest
// (17x17) grid set, and triangle tags from the flat mesh's slopes. Its mesh is the flat grid.
Result<Displacement> createDisplacement(std::span<const DVec3> corners, int power, double elevation);

// Recomputes each triangle's walkable and buildable bits from its slope in the mesh, unless the
// triangle's force bit sets that value. Other bits are kept.
Result<void> retagDisplacement(std::span<const DVec3> corners, Displacement& disp);

struct DisplacementPatch {
    std::array<DVec3, 4> corners;
    Displacement disp;
};

// The base-face tolerance for "these vertices are the same point" and "this vertex lies on that
// edge" when matching neighbours.
inline constexpr double kSewBaseTolerance = 0.01;

// Snaps the vertices that neighbouring patches share to their average, as Hammer's Sew does.
// Neighbours are found on the flat base faces: a boundary vertex whose base point is another
// patch's boundary vertex is averaged with it (corners with up to four patches); one that lies
// on another patch's base edge between two of its vertices (a T-edge between different powers)
// is moved onto the line between that patch's two vertices, after their averaging. Only distances
// and offsets change (the base stays put); tags are recomputed. Returns how many patches changed.
Result<std::size_t> sewDisplacements(std::span<DisplacementPatch> patches);

// The worst gap on each pair of patches that share a base edge (at least two matched vertices),
// measured by the same matching as sewDisplacements. Sewn patches measure 0.
struct DisplacementSeam {
    std::size_t a = 0, b = 0;
    double gap = 0.0;
};
Result<std::vector<DisplacementSeam>> displacementSeams(std::span<const DisplacementPatch> patches);

// Apex's Subdivide (D6), a stand-in for Hammer's, whose exact rule is not known here: one
// umbrella smoothing step (each interior vertex moves halfway to the average of its four grid
// neighbours) stored in offsets, and subdiv set to 1. Boundary vertices stay, so sewn edges stay
// sewn, and a flat grid is unchanged. Tags are recomputed.
Result<void> subdivideDisplacement(std::span<const DVec3> corners, Displacement& disp);

// True when nothing but the grid itself would be lost by rebuilding the displacement flat:
// every distance, offset and alpha is 0.
bool displacementIsFlat(const Displacement& disp);

} // namespace apex::geometry
