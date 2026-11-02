#pragma once

#include "core/job_pool.hpp"
#include "lighting/baker.hpp"

#include <chrono>
#include <set>

namespace apex::lighting {

struct BakeSessionOptions {
    BakerOptions baker{.bounces = 2};
    unsigned initialPasses = 4;
    unsigned maxPasses = 64;
};

struct BakeProgress {
    std::uint64_t revision = 0;
    std::vector<unsigned> passes; // Completed whole-scene traversals, direct then bounce levels.
    unsigned level = 0;
    unsigned target = 0;
    bool idle = false;
};

struct BakeFaceUpdate {
    std::size_t face = 0;
    std::vector<Vec3> radiance;
    std::vector<bool> sampled; // Preserve previous atlas luxels until their first new direct sample.
};
struct BakePublication {
    BakeProgress progress;
    std::vector<BakeFaceUpdate> faces; // Only rows touched by this slice.
};

// Worker-owned, no Qt/GPU. All methods exclusive, just like Baker. Restart replaces the entire
// immutable scene/accel. Camera order is centre-distance, index breaks ties; it changes only at
// whole-scene pass boundaries so moving the camera cannot starve a traversal.
// Order: direct K0, each bounce K0, direct maximum, each bounce maximum, idle.
class BakeSession {
public:
    BakeSession(BakeScene scene, BakeSessionOptions options = {}, std::uint64_t revision = 0);
    void restart(BakeScene scene, BakeSessionOptions options, std::uint64_t revision);
    void setCamera(DVec3 camera) { camera_ = camera; }
    static std::vector<std::size_t> cameraOrder(std::span<const BakeFace> faces, DVec3 camera);
    // One bounded luxel operation, useful for deterministic scheduling/tests.
    void advance(std::size_t budget);
    // Adaptive small operations targeting 40 ms, cancellation checked between operations.
    BakePublication tick(CancelToken cancel = CancelToken{}, std::chrono::milliseconds time = std::chrono::milliseconds(40));
    const BakeProgress& progress() const { return progress_; }
    const BakeScene& scene() const { return baker_->scene(); }

private:
    std::unique_ptr<Baker> baker_;
    BakeSessionOptions options_;
    BakeProgress progress_;
    DVec3 camera_{0};
    std::vector<std::size_t> order_;
    std::size_t nextFace_ = 0, budget_ = 16;
    bool refining_ = false;
    std::set<std::size_t> changed_;
    void nextStage();
};

} // namespace apex::lighting
