#pragma once

#include "geometry/convex_polyhedron.hpp"
#include "geometry/displacement.hpp"

namespace apex::lighting {

// BSP v25 brush subdivision span, before floor/ceil and the extra endpoint sample.
inline constexpr int kBrushLightmapSpan = 128;

struct LuxelChart {
    std::array<int, 2> mins{};
    std::array<int, 2> extents{}; // BSP m_LightmapTextureSizeInLuxels; sample counts are extents + 1.
    std::array<int, 2> dimensions{};
    std::array<DVec3, 2> worldToLuxel{};
    DVec2 offset{0.0};
    DVec3 origin{0.0}; // World position of local sample (0, 0), including mins.
    std::array<DVec3, 2> luxelToWorld{};

    DVec3 worldAt(DVec2 sample) const {
        return origin + sample.x * luxelToWorld[0] + sample.y * luxelToWorld[1];
    }
};

struct ChartedFace {
    // Face indices address this fragment's vertices; all source attributes are retained.
    std::vector<DVec3> vertices;
    geometry::Face face;
    LuxelChart chart;
};

struct FaceCharting {
    LuxelChart chart; // Original face, even when it requires subdivision.
    std::vector<ChartedFace> fragments; // One unchanged loop when no split is needed.
};

// Pure charting of a valid convex, planar brush face. No material flags, BSP/CSG cuts or
// displacement handling. Default compiler settings: scale 0 becomes 16, negatives become 1.
// Invalid indices, nonfinite data, singular axes or unrepresentable extents throw invalid_argument.
FaceCharting chartFace(const geometry::Face& face, std::span<const DVec3> vertices);

// The chart of a compiled BSP face: texinfo lightmap vectors and offsets (already divided by the
// lightmap scale), the face's stored mins and size in luxels, and its plane. Throws invalid_argument.
LuxelChart bspChart(std::array<DVec3, 2> worldToLuxel, DVec2 offset, std::array<int, 2> mins,
                    std::array<int, 2> extents, const Plane& plane);

// Displaced sides (plan 0.8.5; docs/plans/probe-results/displacement_lighting_0_8_5_p0.md). VBSP charts
// a displacement on its own parametric grid, not by projection: luxel (s, t) samples the displaced
// surface at grid fraction (row t / extents[1], column s / extents[0]), edge to edge, from the start
// corner. One chart per side; no 128-luxel subdivision.

// The base quad in Source grid order: corner 0 is the one nearest `start`, rows run 0 -> 1 and columns
// 0 -> 3 (geometry::displacementMesh). `corners` is counter-clockwise from outside (displacementCorners).
std::array<DVec3, 4> displacementGridCorners(std::span<const DVec3> corners, DVec3 start);

// VBSP's luxel size for a displacement: int(1.0f / |uaxis / lightmapScale|), in float, so scales such as
// 7, 13, 14 and 15 truncate to one less. lightmapScale 0 is 16 and negatives are 1, as in chartFace.
int displacementLuxelScale(const geometry::Face& face);

// mins, worldToLuxel and offset are the flat face's (chartFace); extents[0] = floor(longer column edge
// (0 -> 3, 1 -> 2) / luxel scale) + 1 and extents[1] likewise along the rows. origin and luxelToWorld
// stay the planar inverse and do not locate luxels. Throws invalid_argument like chartFace.
LuxelChart chartDisplacement(const geometry::Face& face, std::span<const DVec3> vertices,
                             const std::array<DVec3, 4>& gridCorners);

struct SurfacePoint {
    DVec3 position{0.0};
    DVec3 normal{0.0, 0.0, 1.0}; // Unit.
};

// Area-weighted vertex normals of the displaced mesh (the viewport's shading normals).
std::vector<DVec3> displacementNormals(const geometry::DisplacementMesh& mesh);

// The point at grid fraction (row, column) in [0, 1]^2 on the tessellated surface (VBSP's triangle split),
// with the vertex normals interpolated across that triangle.
SurfacePoint displacementSurfaceAt(const geometry::DisplacementMesh& mesh, std::span<const DVec3> normals,
                                   DVec2 fraction);

// The baker's luxel-corner points for a chart of `extents`: (extents[0] + 2) x (extents[1] + 2) points,
// row-major, where point (x, y) is at luxel coordinate (x - 0.5, y - 0.5) clamped to the grid and kept
// 1 world unit inside its edges (clampToFace's inset). Luxel (s, t)'s corner (dx, dy) is point (s + dx, t + dy).
std::vector<SurfacePoint> displacementLattice(const geometry::DisplacementMesh& mesh,
                                              const std::array<DVec3, 4>& gridCorners, std::array<int, 2> extents);

} // namespace apex::lighting
