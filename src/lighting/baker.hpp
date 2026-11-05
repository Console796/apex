#pragma once

#include "lighting/bake_scene.hpp"

#include <limits>

namespace apex::lighting {

struct BakerOptions {
    std::uint64_t seed = 0;
    unsigned threads = 3;
    unsigned skyRays = 64; // Upper-hemisphere strata per lifted sample; at least one.
    unsigned bounces = 0;
    unsigned bounceRays = 32; // Cosine-weighted hemisphere samples per lifted point per bounce.
    double bounceReflectivity = 1; // Scales every albedo in bounce transport, in [0,1]; direct is unaffected.
};

// The bounce reflectivity at which Baked's interreflection matches VRAD's patch radiosity (P0 of 0.7.7).
inline constexpr double kVradBounceReflectivity = 0.97;

// Luxel-corner sample points past the face's edge would sample against or behind the neighbouring
// surface and darken concave creases. Points closer than 1 unit inside the edge (or outside) move to
// the nearest boundary point, then up to 1 unit towards the face's centre; the rest are unchanged.
DVec3 clampToFace(const BakeFace& face, DVec3 point);

struct BakeBuffer {
    std::array<int, 2> dimensions{};
    std::vector<Vec3> radiance; // RGBF32 sums, row-major (t * width + s).
    std::vector<std::uint64_t> samples; // Number of lifted point samples, four per pass.
};

struct BakeStep {
    std::size_t luxels = 0;
    bool passComplete = false;
};

class Baker {
public:
    explicit Baker(BakeScene scene, BakerOptions options = {});

    // Synchronous, parallel work on at most budget luxels, never beyond one traversal. Empty faces
    // selects all. Repeat with the same selection to resume a budget-limited pass. Changing selection
    // starts a new traversal, retaining all sums/counts; duplicate/out-of-range faces throw.
    // Seeds depend on each luxel's own count, so worker scheduling and budget partitioning agree.
    // All methods are exclusive; callbacks and the accel must support concurrent read-only queries.
    BakeStep step(std::size_t budget = std::numeric_limits<std::size_t>::max(),
                  std::span<const std::size_t> faces = {});
    // Run after settling direct, then each preceding bounce level (1..options.bounces) in order.
    // Same budget/selection semantics as step. Each new traversal snapshots resolveLevel(level-1);
    // budget-limited continuations retain that snapshot. Levels store successive contributions,
    // not cumulative totals. Settle upstream levels before accumulating downstream passes.
    BakeStep bounce(unsigned level, std::size_t budget = std::numeric_limits<std::size_t>::max(),
                    std::span<const std::size_t> faces = {});
    const std::vector<BakeBuffer>& buffers(unsigned level = 0) const;
    std::vector<std::vector<Vec3>> resolve() const; // Direct + bounce means; zero if unsampled.
    std::vector<std::vector<Vec3>> resolveLevel(unsigned level) const; // 0 is direct.
    const BakeScene& scene() const { return scene_; }
    // Incremental restart support. adopt installs previously accumulated sums/counts for one face at one
    // level (dimensions must match). releaseBuffers moves a level out and leaves the baker unusable.
    void adopt(unsigned level, std::size_t face, BakeBuffer buffer);
    std::vector<BakeBuffer> releaseBuffers(unsigned level);

private:
    DVec3 sample(std::size_t face, std::size_t luxel) const;
    DVec3 sampleBounce(unsigned level, std::size_t face, std::size_t luxel) const;
    double skyVisibility(DVec3 point, DVec3 normal, std::uint64_t key) const;
    BakeStep stepLevel(unsigned level, std::size_t budget, std::span<const std::size_t> faces);

    struct Pass {
        std::vector<std::size_t> selection;
        std::size_t nextFace = 0, nextLuxel = 0;
        bool complete = true;
        std::vector<std::vector<Vec3>> source;
    };

    BakeScene scene_;
    BakerOptions options_;
    std::vector<BakeBuffer> buffers_;
    std::vector<std::vector<BakeBuffer>> bounceBuffers_;
    std::vector<Pass> passes_;
};

} // namespace apex::lighting
