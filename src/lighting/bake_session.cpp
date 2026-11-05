#include "lighting/bake_session.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace apex::lighting {
BakeState seedState(const BakeScene& scene, std::vector<FaceKey> keys,
                    std::span<const std::vector<Vec3>> radiance, std::span<const char> known,
                    const BakeSessionOptions& options) {
    const auto count = scene.faces.size();
    if (!options.maxPasses || !options.initialPasses || radiance.size() != count || known.size() != count)
        throw std::invalid_argument("seedState: invalid inputs");
    if (keys.empty()) keys = faceKeys(scene.faces);
    if (keys.size() != count) throw std::invalid_argument("seedState: key count mismatch");
    BakeState state;
    state.faces = scene.faces;
    state.lights = scene.lights;
    state.materials = scene.materials;
    state.keys = std::move(keys);
    state.baker = options.baker;
    state.unpublished.assign(count, 1);
    state.buffers.resize(std::size_t(options.baker.bounces) + 1);
    state.passes.assign(state.buffers.size(), std::vector<unsigned>(count, 0));
    const auto samples = std::uint64_t{4} * options.maxPasses;
    for (std::size_t i = 0; i < count; ++i) {
        const auto dims = scene.faces[i].chart.dimensions;
        if (dims[0] < 1 || dims[1] < 1) throw std::invalid_argument("seedState: invalid chart dimensions");
        const auto size = std::size_t(dims[0]) * std::size_t(dims[1]);
        if (known[i] && radiance[i].size() != size) throw std::invalid_argument("seedState: luxel count mismatch");
        for (std::size_t level = 0; level < state.buffers.size(); ++level) {
            auto& buffer = state.buffers[level].emplace_back();
            buffer.dimensions = dims;
            buffer.radiance.assign(size, Vec3{0});
            buffer.samples.assign(size, known[i] ? samples : 0);
            if (known[i]) {
                state.passes[level][i] = options.maxPasses;
                if (level == 0) for (std::size_t j = 0; j < size; ++j)
                    buffer.radiance[j] = radiance[i][j] * float(samples);
            }
        }
    }
    return state;
}

BakeSession::BakeSession(BakeScene scene, BakeSessionOptions options, std::uint64_t revision,
                         std::vector<FaceKey> keys) {
    reset(std::move(scene), options, revision, std::move(keys));
}

void BakeSession::restart(BakeScene scene, BakeSessionOptions options, std::uint64_t revision,
                          std::vector<FaceKey> keys) {
    std::lock_guard lock(mutex_);
    reset(std::move(scene), options, revision, std::move(keys));
}

void BakeSession::reset(BakeScene scene, const BakeSessionOptions& options, std::uint64_t revision,
                        std::vector<FaceKey> keys) {
    if (!options.maxPasses || !options.initialPasses) throw std::invalid_argument("BakeSession: zero passes");
    options_ = options;
    keys_ = keys.empty() ? faceKeys(scene.faces) : std::move(keys);
    if (keys_.size() != scene.faces.size()) throw std::invalid_argument("BakeSession: key count mismatch");
    baker_ = std::make_unique<Baker>(std::move(scene), options.baker);
    progress_ = {revision, std::vector<unsigned>(options.baker.bounces + 1, 0), 0,
                 std::min(options.initialPasses, options.maxPasses), baker_->scene().faces.empty()};
    facePasses_.assign(options.baker.bounces + 1, std::vector<unsigned>(baker_->scene().faces.size(), 0));
    order_.clear();
    nextFace_ = 0;
    budget_ = 16;
    refining_ = ordered_ = focusing_ = false;
    focus_.clear();
    changed_.clear();
}

BakeState BakeSession::takeState() {
    std::lock_guard lock(mutex_);
    BakeState state;
    if (!baker_) return state;
    const auto& scene = baker_->scene();
    state.faces = scene.faces;
    state.lights = scene.lights;
    state.materials = scene.materials;
    state.keys = std::move(keys_);
    state.baker = options_.baker;
    state.passes = std::move(facePasses_);
    state.unpublished.assign(scene.faces.size(), 0);
    for (const auto face : changed_) state.unpublished[face] = 1;
    for (unsigned level = 0; level <= options_.baker.bounces; ++level)
        state.buffers.push_back(baker_->releaseBuffers(level));
    baker_.reset();
    keys_.clear();
    facePasses_.clear();
    order_.clear();
    changed_.clear();
    progress_.idle = true;
    return state;
}

BakeDiff BakeSession::restart(BakeState previous, BakeScene scene, BakeSessionOptions options,
                              std::uint64_t revision, std::vector<FaceKey> keys) {
    std::lock_guard lock(mutex_);
    if (keys.empty()) keys = faceKeys(scene.faces);
    const BakeSceneView next{scene.faces, scene.lights, scene.materials, keys};
    const auto& a = previous.baker;
    const auto& b = options.baker;
    const bool sameSettings = a.seed == b.seed && a.skyRays == b.skyRays && a.bounces == b.bounces &&
                              a.bounceRays == b.bounceRays;
    BakeDiff diff;
    if (previous.empty() || previous.buffers.size() != std::size_t(b.bounces) + 1 || !sameSettings) {
        diff = diffScenes({}, next); // Sized all-dirty, full.
        diff.reason = previous.empty() ? "no previous state" : "baker settings changed";
    } else diff = diffScenes(previous.view(), next, options.rebakeReach);
    if (!diff.full && options.freezeBounceDirty && options.rebakeBounceRing)
        for (std::size_t i = 0; i < diff.dirty.size(); ++i)
            if (diff.bounceDirty[i] && !diff.dirty[i]) { diff.dirty[i] = 1; ++diff.dirtyCount; }
    reset(std::move(scene), options, revision, std::move(keys));
    if (diff.full) return diff;

    const auto faces = baker_->scene().faces.size();
    const unsigned levels = options.baker.bounces + 1;
    for (std::size_t i = 0; i < faces; ++i) {
        const auto old = diff.oldOfNew[i];
        if (old < 0 || diff.dirty[i]) continue;
        for (unsigned level = 0; level < levels; ++level) {
            if (level > 0 && diff.bounceDirty[i] && !options.freezeBounceDirty) break; // Direct stays, bounce restarts.
            auto& buffer = previous.buffers[level][std::size_t(old)];
            try { baker_->adopt(level, i, std::move(buffer)); }
            catch (const std::invalid_argument&) { break; } // Charts disagree: keep this face fresh.
            facePasses_[level][i] = previous.passes[level][std::size_t(old)];
        }
        if (previous.unpublished[std::size_t(old)]) changed_.insert(i);
    }
    const auto dirtyFaces = std::count(diff.dirty.begin(), diff.dirty.end(), char(1));
    if (dirtyFaces && std::size_t(dirtyFaces) < faces) {
        focus_.assign(diff.dirty.begin(), diff.dirty.end());
        focusing_ = true;
    }
    for (unsigned level = 0; level < levels; ++level)
        progress_.passes[level] = facePasses_[level].empty() ? 0
            : *std::min_element(facePasses_[level].begin(), facePasses_[level].end());
    return diff;
}

std::vector<std::size_t> BakeSession::cameraOrder(std::span<const BakeFace> faces, DVec3 camera) {
    std::vector<double> distances;
    for (const auto& face : faces) {
        DVec3 centre{0};
        for (const auto p : face.vertices) centre += p;
        if (!face.vertices.empty()) centre /= double(face.vertices.size());
        const auto delta = centre - camera;
        distances.push_back(glm::dot(delta, delta));
    }
    std::vector<std::size_t> order(faces.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return distances[a] < distances[b]; });
    return order;
}

void BakeSession::nextStage() {
    if (progress_.passes[progress_.level] < progress_.target) return;
    const unsigned top = focusing_ ? std::min(1u, options_.baker.bounces) : options_.baker.bounces;
    if (progress_.level < top) ++progress_.level;
    else if (!refining_ && progress_.target < options_.maxPasses) {
        refining_ = true;
        progress_.level = 0;
        progress_.target = options_.maxPasses;
    } else if (focusing_) { // Dirty faces done: restart the ordinary schedule over every face.
        focusing_ = refining_ = false;
        progress_.level = 0;
        progress_.target = std::min(options_.initialPasses, options_.maxPasses);
    } else progress_.idle = true;
}

void BakeSession::finishTraversal() {
    const auto& counts = facePasses_[progress_.level];
    unsigned lowest = std::numeric_limits<unsigned>::max();
    for (std::size_t face = 0; face < counts.size(); ++face)
        if (!focusing_ || focus_[face]) lowest = std::min(lowest, counts[face]);
    progress_.passes[progress_.level] = lowest;
    nextFace_ = 0;
    order_.clear();
    ordered_ = false;
    nextStage();
}

void BakeSession::advance(std::size_t budget) {
    std::lock_guard lock(mutex_);
    advanceLocked(budget);
}

void BakeSession::advanceLocked(std::size_t budget) {
    if (budget == 0) return;
    while (!ordered_) {
        if (progress_.idle) return;
        order_ = cameraOrder(scene().faces, camera_);
        const auto& done = facePasses_[progress_.level];
        std::erase_if(order_, [&](auto face) {
            return done[face] >= progress_.target || (focusing_ && !focus_[face]);
        });
        if (!focusing_ && !focus_.empty())
            std::stable_partition(order_.begin(), order_.end(), [&](auto face) { return focus_[face] != 0; });
        nextFace_ = 0;
        ordered_ = true;
        if (order_.empty()) finishTraversal(); // Every face already at this level's target: next stage.
    }
    if (progress_.idle) return;
    const auto face = order_[nextFace_];
    const std::array selection{face};
    const auto step = progress_.level == 0 ? baker_->step(budget, selection)
                                         : baker_->bounce(progress_.level, budget, selection);
    if (step.luxels) changed_.insert(face);
    if (step.passComplete) {
        ++facePasses_[progress_.level][face];
        if (++nextFace_ == order_.size()) finishTraversal();
    }
}

BakePublication BakeSession::tick(CancelToken cancel, std::chrono::milliseconds time) {
    std::lock_guard lock(mutex_);
    const auto end = std::chrono::steady_clock::now() + time;
    while (!cancel.cancelled() && !progress_.idle && std::chrono::steady_clock::now() < end) {
        const auto start = std::chrono::steady_clock::now();
        advanceLocked(budget_);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // Aim below the full slice duration to bound cancellation latency, even at expensive levels.
        budget_ = std::clamp(std::size_t(double(budget_) * 5.0 / std::max(ms, 0.01)), std::size_t{1}, std::size_t{256});
    }
    BakePublication publication{progress_, {}};
    // A cancelled tick publishes nothing; its faces stay in changed_ for takeState to report as unpublished.
    if (cancel.cancelled() || !baker_) return publication;
    if (!changed_.empty()) {
        auto resolved = baker_->resolve();
        for (const auto face : changed_) {
            auto& update = publication.faces.emplace_back();
            update.face = face;
            update.radiance = std::move(resolved[face]);
            for (const auto count : baker_->buffers()[face].samples) update.sampled.push_back(count != 0);
        }
        changed_.clear();
    }
    return publication;
}
} // namespace apex::lighting
