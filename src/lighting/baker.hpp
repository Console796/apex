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
};

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
