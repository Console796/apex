#include "lighting/bake_session.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace apex::lighting {
BakeSession::BakeSession(BakeScene scene, BakeSessionOptions options, std::uint64_t revision) {
    restart(std::move(scene), options, revision);
}

void BakeSession::restart(BakeScene scene, BakeSessionOptions options, std::uint64_t revision) {
    if (!options.maxPasses || !options.initialPasses) throw std::invalid_argument("BakeSession: zero passes");
    options_ = options;
    baker_ = std::make_unique<Baker>(std::move(scene), options.baker);
    progress_ = {revision, std::vector<unsigned>(options.baker.bounces + 1, 0), 0,
                 std::min(options.initialPasses, options.maxPasses), baker_->scene().faces.empty()};
    order_.clear();
    nextFace_ = 0;
    budget_ = 16;
    refining_ = false;
    changed_.clear();
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
    if (progress_.level < options_.baker.bounces) ++progress_.level;
    else if (!refining_ && progress_.target < options_.maxPasses) {
        refining_ = true;
        progress_.level = 0;
        progress_.target = options_.maxPasses;
    } else progress_.idle = true;
}

void BakeSession::advance(std::size_t budget) {
    if (progress_.idle || budget == 0) return;
    if (order_.empty()) order_ = cameraOrder(scene().faces, camera_);
    const auto face = order_[nextFace_];
    const std::array selection{face};
    const auto step = progress_.level == 0 ? baker_->step(budget, selection)
                                         : baker_->bounce(progress_.level, budget, selection);
    if (step.luxels) changed_.insert(face);
    if (step.passComplete && ++nextFace_ == order_.size()) {
        ++progress_.passes[progress_.level];
        nextFace_ = 0;
        order_.clear();
        nextStage();
    }
}

BakePublication BakeSession::tick(CancelToken cancel, std::chrono::milliseconds time) {
    const auto end = std::chrono::steady_clock::now() + time;
    while (!cancel.cancelled() && !progress_.idle && std::chrono::steady_clock::now() < end) {
        const auto start = std::chrono::steady_clock::now();
        advance(budget_);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // Aim below the full slice duration to bound cancellation latency, even at expensive levels.
        budget_ = std::clamp(std::size_t(double(budget_) * 5.0 / std::max(ms, 0.01)), std::size_t{1}, std::size_t{256});
    }
    BakePublication publication{progress_, {}};
    if (cancel.cancelled()) { changed_.clear(); return publication; }
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
