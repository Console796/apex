#pragma once

#include "lighting/bake_scene.hpp"

#include <limits>

namespace apex::lighting {

struct BakerOptions {
    std::uint64_t seed = 0;
    unsigned threads = 3;
    unsigned skyRays = 64; // Upper-hemisphere strata per lifted sample; at least one.
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
    const std::vector<BakeBuffer>& buffers() const { return buffers_; }
    std::vector<std::vector<Vec3>> resolve() const; // Means, zero for unsampled luxels.
    const BakeScene& scene() const { return scene_; }

private:
    DVec3 sample(std::size_t face, std::size_t luxel) const;
    double skyVisibility(DVec3 point, DVec3 normal, std::uint64_t key) const;

    BakeScene scene_;
    BakerOptions options_;
    std::vector<BakeBuffer> buffers_;
    std::vector<std::size_t> selection_;
    std::size_t nextFace_ = 0, nextLuxel_ = 0;
    bool passComplete_ = true;
};

} // namespace apex::lighting
