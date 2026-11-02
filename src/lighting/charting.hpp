#pragma once

#include "geometry/convex_polyhedron.hpp"

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

} // namespace apex::lighting
