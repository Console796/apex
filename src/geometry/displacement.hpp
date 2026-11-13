#pragma once

#include "core/math.hpp"
#include "core/result.hpp"
#include "geometry/convex_polyhedron.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::geometry {

// A side's displacement (plan 0.8 D1), parsed from its VMF dispinfo block by document/.
// Per-vertex arrays are row-major with size() * size() entries; triangleTags holds two
// entries per grid quad, in VBSP's order. Values are kept as written: alphas outside 0..255
// are not clamped (VBSP copies them unclamped into DISP_VERTS).
struct Displacement {
    static constexpr int kMinPower = 2, kMaxPower = 4;

    int power = 0;
    DVec3 startPosition{0.0};
    int flags = 0;
    double elevation = 0.0; // Kept for Hammer; VBSP ignores it (see displacementMesh).
    int subdiv = 0;
    std::vector<DVec3> normals;
    std::vector<double> distances;
    std::vector<DVec3> offsets;
    std::vector<DVec3> offsetNormals; // Kept for Hammer; VBSP ignores them.
    std::vector<double> alphas;
    std::vector<int> triangleTags;
    std::vector<bool> allowedVerts; // One bit per stored bit (Hammer pads past the grid); empty if absent.

    int size() const { return (1 << power) + 1; }
    std::size_t vertexCount() const { return static_cast<std::size_t>(size()) * static_cast<std::size_t>(size()); }
    std::size_t triangleCount() const { return 2 * static_cast<std::size_t>(size() - 1) * static_cast<std::size_t>(size() - 1); }
    // The first inconsistency (power range, array sizes, nonfinite values), or nullopt.
    std::optional<std::string> problem() const;
    bool operator==(const Displacement&) const = default;
};

struct DisplacementMesh {
    int size = 0;                                       // Vertices per row.
    std::vector<DVec3> positions;                       // Row-major, as Displacement.
    std::vector<double> alphas;
    std::vector<std::array<std::uint32_t, 3>> triangles; // VBSP's order; counter-clockwise from outside.
    std::vector<int> tags;                              // Parallel to triangles.
};

// The mesh VBSP builds (D2). corners are the base face's four corners counter-clockwise from
// outside (geometry::Face order). In Source's clockwise order c0..c3 starting at the corner
// nearest startPosition, row i runs from c0 towards c1 and column j from c0 towards c3; the
// flat point is the bilinear blend, and each vertex adds normal * distance + offset.
// Elevation and offset normals are not used: VBSP leaves them out of DISP_VERTS.
Result<DisplacementMesh> displacementMesh(std::span<const DVec3> corners, const Displacement& disp);

// A face's four corners in its own order, skipping collinear subdivision vertices.
Result<std::array<DVec3, 4>> displacementCorners(const ConvexPolyhedron& poly, const Face& face);

// D4: carry a displacement through p -> linear * p + translation so that the mesh on the
// transformed base face is the transformed mesh. An orientation-reversing map transposes the
// grid (applyToSolid reverses the face loop, which swaps Source's row and column edges).
// Throws std::invalid_argument for an invalid displacement or a singular map.
Displacement transformDisplacement(const Displacement& disp, const DMat3& linear, DVec3 translation);

} // namespace apex::geometry
