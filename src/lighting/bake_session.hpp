#pragma once

#include "core/job_pool.hpp"
#include "lighting/bake_diff.hpp"
#include "lighting/baker.hpp"

#include <chrono>
#include <mutex>
#include <set>

namespace apex::lighting {

struct BakeSessionOptions {
    BakerOptions baker{.bounces = 8, .bounceReflectivity = kVradBounceReflectivity};
    unsigned initialPasses = 4;
    unsigned maxPasses = 4;
    bool freezeBounceDirty = false;
    double rebakeReach = 1; // Scales the diff's bounds (diffScenes `reach`).
    // With freezeBounceDirty: rebake the bounce ring fully instead of freezing it. Its frozen light already
    // holds every bounce, so it restarts from direct rather than keep it and count the bounces twice.
    bool rebakeBounceRing = false;
    double advanceMilliseconds = 5.0;
    std::size_t maxBatchFaces = std::numeric_limits<std::size_t>::max();
};

struct BakeProgress {
    std::uint64_t revision = 0;
    std::vector<unsigned> passes; // Completed traversals per level (the minimum over faces), direct then bounces.
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

// What a retired session hands to its successor: the converged sums and per-face pass counts, plus
// the scene description needed to diff against (no occlusion data).
struct BakeState {
    std::vector<BakeFace> faces;
    std::vector<Light> lights;
    std::vector<std::string> materials;
    std::vector<FaceKey> keys;
    std::vector<std::vector<BakeBuffer>> buffers; // [level][face]
    std::vector<std::vector<unsigned>> passes; // [level][face]: completed traversals of that face.
    std::vector<char> unpublished; // [face]: advanced since the last publication.
    BakerOptions baker;
    bool empty() const { return faces.empty(); }
    BakeSceneView view() const { return {faces, lights, materials, keys}; }
};

BakeState seedState(const BakeScene& scene, std::vector<FaceKey> keys,
                    std::span<const std::vector<Vec3>> radiance, std::span<const char> known,
                    const BakeSessionOptions& options);

// Worker-owned, no Qt. Methods are serialised by an internal mutex, so a cancelled tick can
// still be finishing while takeState waits. Restart replaces the entire immutable scene/accel.
// Camera order is centre-distance, index breaks ties; it changes only at traversal boundaries so
// moving the camera cannot starve a traversal.
// Order: direct K0, each bounce K0, direct maximum, each bounce maximum, idle. A traversal at a level
// visits only faces whose own pass count there is below the target, in camera order, so faces kept
// from a previous bake (counts already at target) are skipped.
// After an incremental restart the order is led by the edit: first the dirty faces alone run direct and
// the first bounce level (both at K0 and at the maximum; the first bounce reads only direct, which is final
// for every face by then), then the usual order follows with dirty faces ahead of the rest in each traversal,
// which refreshes the bounce-dirty neighbours and the deeper bounce levels. Clean faces keep showing their old
// radiance meanwhile.
class BakeSession {
public:
    using KernelFactory = std::function<std::shared_ptr<BakeKernel>(const BakeScene&, const BakerOptions&)>;
    BakeSession(BakeScene scene, BakeSessionOptions options = {}, std::uint64_t revision = 0,
                std::vector<FaceKey> keys = {}, KernelFactory factory = {});
    // Everything from zero. Keys default to faceKeys(scene.faces).
    void restart(BakeScene scene, BakeSessionOptions options, std::uint64_t revision, std::vector<FaceKey> keys = {});
    // Incremental restart: diff against `previous`, keep sums and counts of clean faces (all levels) and of
    // bounce-only-dirty faces (direct only), restart the rest from zero. Falls back to a full restart (diff.full)
    // when the diff says so or baker settings differ. Returns the diff used.
    BakeDiff restart(BakeState previous, BakeScene scene, BakeSessionOptions options, std::uint64_t revision,
                     std::vector<FaceKey> keys = {});
    // Moves the converged state out and retires this session (idle, empty). Safe against a concurrent tick.
    BakeState takeState();
    void setCamera(DVec3 camera) { camera_ = camera; }
    static std::vector<std::size_t> cameraOrder(std::span<const BakeFace> faces, DVec3 camera);
    // One bounded luxel operation, useful for deterministic scheduling/tests.
    void advance(std::size_t budget);
    // Adaptive small operations targeting 40 ms, cancellation checked between operations.
    BakePublication tick(CancelToken cancel = CancelToken{}, std::chrono::milliseconds time = std::chrono::milliseconds(40));
    const BakeProgress& progress() const { return progress_; }
    const BakeScene& scene() const { return baker_->scene(); }
    const Baker& baker() const { return *baker_; }
    bool gpu() const { return gpu_; }
    const std::string& kernelError() const { return kernelError_; }
    const std::vector<unsigned>& facePasses(unsigned level) const { return facePasses_.at(level); }

private:
    std::mutex mutex_;
    std::unique_ptr<Baker> baker_;
    KernelFactory factory_;
    bool gpu_ = false;
    std::string kernelError_;
    BakeSessionOptions options_;
    BakeProgress progress_;
    DVec3 camera_{0};
    std::vector<std::size_t> order_;
    std::vector<FaceKey> keys_;
    std::vector<std::vector<unsigned>> facePasses_; // [level][face]
    std::size_t nextFace_ = 0, budget_ = 16, batchLuxels_ = 0;
    std::vector<std::size_t> batch_;
    bool refining_ = false, ordered_ = false;
    bool focusing_ = false;    // Incremental restart: first converge the dirty faces alone (direct and first bounce).
    std::vector<char> focus_;  // [face]: dirty in the last incremental restart; those lead every later traversal.
    std::set<std::size_t> changed_;
    void nextStage();
    void advanceLocked(std::size_t budget);
    void finishTraversal();
    void reset(BakeScene scene, const BakeSessionOptions& options, std::uint64_t revision, std::vector<FaceKey> keys);
};

} // namespace apex::lighting
