#pragma once

namespace apex::geometry {

// Source units are approximately inches. These absolute double tolerances also apply at
// +/-65536: they exceed floating-point clipping noise while preserving sub-grid geometry.
inline constexpr double kPlanarEps = 1e-6;          // Signed distance, in units.
inline constexpr double kPointMergeEps = 1e-7;      // Euclidean vertex welding distance.
inline constexpr double kPointDataTrustEps = 1e-3;  // Accept small plane serialization drift.
inline constexpr double kNormalMatchEps = 1e-6;     // Euclidean distance between unit normals.
inline constexpr double kMinEdgeLength = 1e-5;     // Units; thinner features are invalid.
inline constexpr double kMinFaceArea = 1e-10;       // Square units.

} // namespace apex::geometry
