#pragma once

#include "lighting/bake_scene.hpp"

#include <span>
#include <string>

namespace apex::lighting {

// Stable identity of a charted face across rebuilds: source side plus the fragment ordinal among the
// faces that share it. `scope` is 0 for ordinary faces; callers whose ids are renumbered per rebuild
// (instance expansion) put a hash of the stable identity there and in solid/side.
struct FaceKey {
    std::uint64_t scope = 0, solid = 0;
    std::uint32_t side = 0, fragment = 0;
    auto operator<=>(const FaceKey&) const = default;
};
std::vector<FaceKey> faceKeys(std::span<const BakeFace> faces);

// Bounds used to decide what an edit can affect. See docs/plans/probe-results/incremental_bake_0_7_4.md.
inline constexpr double kLightThreshold = 1.0 / 256.0; // Linear HDR below which a light is treated as not reaching.
inline constexpr double kGeometryMinRadius = 128;      // Near-field reach of a changed occluder (shadows, sky, bounce).
inline constexpr double kGeometryScale = 8;            // ...or this many occluder radii, whichever is larger.
inline constexpr double kBounceRadius = 512;           // Bounce re-runs this far (sphere gap) from any dirty face.

// A scene, as the diff sees it. Spans are borrowed; keys.size() == faces.size().
struct BakeSceneView {
    std::span<const BakeFace> faces;
    std::span<const Light> lights;
    std::span<const std::string> materials;
    std::span<const FaceKey> keys;
};

struct BakeDiff {
    bool full = false; // Unbounded or unusable change: restart everything.
    std::string reason; // Why `full`.
    std::vector<int> oldOfNew; // Per new face: matching previous face (same key), else -1.
    std::vector<char> dirty; // Per new face: direct lighting restarts from zero.
    std::vector<char> bounceDirty; // Per new face: bounce levels restart (superset of dirty).
    std::size_t dirtyCount = 0, bounceDirtyCount = 0;
};

BakeDiff diffScenes(const BakeSceneView& before, const BakeSceneView& after);

} // namespace apex::lighting
