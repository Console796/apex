#pragma once

// Shadow tile planning (D13): which lights get atlas tiles, at what size, and when a light keeps its tile.
// Pure CPU, so the hysteresis is unit-testable without a GPU.

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "render/shadows.hpp"

namespace apex::render {

inline constexpr double kShadowLeaveSeconds = 1.0; // An out-of-view light keeps its tile this long (D13).
inline constexpr double kShadowHysteresis = 2.0;   // Contribution factor before a light re-tiers (D13).

struct ShadowRequest {
    std::uint64_t key = 0;
    std::uint32_t views = 0;   // 1 spot, 6 point, kSunCascades sun.
    std::uint32_t maxSize = 0; // Tile size at full contribution.
    double contribution = 0;   // Relative screen share; larger is more important.
    bool inView = true;
    bool pinned = false; // The sun: allocated first and never evicted.
};

struct ShadowGrant {
    bool shadowed = false;
    std::uint32_t views = 0;
    std::array<ShadowRect, 6> rects{};
};

// Owner key: the mirror's stable id, else a hash of the pose (stable while the light is not edited).
inline std::uint64_t shadowKey(const lighting::Light& light) {
    if (light.id) return light.id;
    std::uint64_t h = 0xCBF29CE484222325ull ^ std::uint64_t(light.kind);
    const auto mix = [&](double v) { h = (h ^ std::bit_cast<std::uint64_t>(v)) * 0x100000001B3ull; };
    for (int i = 0; i < 3; ++i) { mix(light.origin[i]); mix(light.direction[i]); }
    return h | 1;
}

// Contribution is the squared ratio of range to camera distance: a light's screen share, to within a constant.
inline double shadowContribution(double range, double distance) {
    const double ratio = range / std::max(distance, 8.0);
    return std::clamp(ratio * ratio, 1e-4, 16.0);
}

// Each factor of four below full contribution halves the tile: the tile's texel density then matches the screen.
inline std::uint32_t shadowTierSize(std::uint32_t maxSize, double contribution) {
    std::uint32_t size = maxSize;
    while (size > kMinShadowTile && contribution < 1.0) { size /= 2; contribution *= 4.0; }
    return size;
}

// Planes of the camera frustum's four sides from the camera-relative viewProjection (reversed-Z agnostic).
inline bool sphereInView(const Mat4& viewProjection, Vec3 centre, float radius) {
    const auto row = [&](int i) { return Vec4(viewProjection[0][i], viewProjection[1][i], viewProjection[2][i], viewProjection[3][i]); };
    const Vec4 planes[4] = {row(3) + row(0), row(3) - row(0), row(3) + row(1), row(3) - row(1)};
    for (const Vec4& plane : planes) {
        const float length = glm::length(Vec3(plane));
        if (length > 0 && glm::dot(Vec3(plane), centre) + plane.w < -radius * length) return false;
    }
    return true;
}

// Request for one light, or nothing when it cannot be shadowed (the caller does not count those).
inline std::optional<ShadowRequest> shadowRequest(const lighting::Light& light, DVec3 camera, const Mat4& viewProjection,
                                                  double range) {
    if (!(lighting::hasDirect(light) || lighting::hasSpecular(light))) return std::nullopt;
    ShadowRequest request;
    request.key = shadowKey(light);
    switch (light.kind) {
    case lighting::LightKind::Sun:
        if (!(glm::length(light.direction) > 0)) return std::nullopt;
        request.views = kSunCascades;
        request.maxSize = kMaxShadowTile;
        request.contribution = 1e9;
        request.pinned = true;
        return request;
    case lighting::LightKind::Point:
        request.views = 6;
        request.maxSize = 512;
        break;
    case lighting::LightKind::Spot:
        if (!(glm::length(light.direction) > 0) || !(light.stopdot2 < 1)) return std::nullopt;
        request.views = spotUsesCube(light) ? 6 : 1;
        request.maxSize = request.views == 6 ? 512 : kMaxShadowTile;
        break;
    default: return std::nullopt;
    }
    if (!(range > 0)) return std::nullopt;
    range = std::min(range, 16384.0); // Unbounded lights (constant-only attenuation) count as a map-sized reach.
    request.contribution = shadowContribution(range, glm::length(light.origin - camera));
    request.inView = sphereInView(viewProjection, Vec3(light.origin - camera), float(range));
    return request;
}

class ShadowAllocator {
  public:
    // One grant per request, in order. Lights outside the view keep their tile but are not granted.
    std::vector<ShadowGrant> update(std::span<const ShadowRequest> requests, double now) {
        overflow_ = 0;
        std::map<std::uint64_t, std::size_t> byKey;
        std::vector<std::uint64_t> keys(requests.size());
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto key = requests[i].key;
            while (byKey.contains(key)) key = key * 0x9E3779B97F4A7C15ull + 1; // Coincident duplicates.
            byKey.emplace(key, i);
            keys[i] = key;
        }
        for (auto it = owners_.begin(); it != owners_.end();)
            it = byKey.contains(it->first) ? std::next(it) : release(it);
        std::vector<std::size_t> pending;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            const auto& request = requests[i];
            auto found = owners_.find(keys[i]);
            if (found == owners_.end()) {
                if (request.inView) pending.push_back(i);
                continue;
            }
            Owner& owner = found->second;
            if (!request.inView) {
                if (now - owner.lastSeen > kShadowLeaveSeconds) release(found);
                continue;
            }
            owner.lastSeen = now;
            const double ratio = request.contribution / owner.reference;
            if (ratio <= kShadowHysteresis && ratio >= 1.0 / kShadowHysteresis && owner.views == request.views) continue;
            owner.reference = request.contribution;
            if (owner.views == request.views && owner.size == shadowTierSize(request.maxSize, request.contribution)) continue;
            release(found);
            pending.push_back(i);
        }
        std::ranges::stable_sort(pending, [&](std::size_t a, std::size_t b) {
            return std::pair(requests[a].pinned, requests[a].contribution) > std::pair(requests[b].pinned, requests[b].contribution);
        });
        for (const std::size_t i : pending) place(keys[i], requests[i], now);
        std::vector<ShadowGrant> grants(requests.size());
        for (std::size_t i = 0; i < requests.size(); ++i) {
            const auto found = owners_.find(keys[i]);
            if (found == owners_.end()) {
                if (requests[i].inView) ++overflow_;
                continue;
            }
            if (!requests[i].inView) continue;
            const Owner& owner = found->second;
            grants[i].shadowed = true;
            grants[i].views = owner.views;
            for (std::uint32_t v = 0; v < owner.views; ++v)
                grants[i].rects[v] = {owner.cells[v][0] * kMinShadowTile, owner.cells[v][1] * kMinShadowTile, owner.size};
        }
        return grants;
    }

    // In-view lights that wanted a shadow and did not fit (D7): shown in the status line.
    std::uint32_t overflow() const { return overflow_; }
    std::size_t ownerCount() const { return owners_.size(); }

  private:
    static constexpr std::uint32_t kCells = kShadowAtlasSize / kMinShadowTile;
    using Occupancy = std::bitset<kCells * kCells>;
    struct Owner {
        std::uint32_t views = 0, size = 0;
        std::array<std::array<std::uint32_t, 2>, 6> cells{};
        double reference = 0, lastSeen = 0;
        bool pinned = false;
    };
    using Owners = std::map<std::uint64_t, Owner>;

    void mark(Occupancy& bits, const Owner& owner, bool value) const {
        const std::uint32_t span = owner.size / kMinShadowTile;
        for (std::uint32_t v = 0; v < owner.views; ++v)
            for (std::uint32_t y = 0; y < span; ++y)
                for (std::uint32_t x = 0; x < span; ++x) bits[(owner.cells[v][1] + y) * kCells + owner.cells[v][0] + x] = value;
    }
    Owners::iterator release(Owners::iterator it) {
        mark(occupied_, it->second, false);
        return owners_.erase(it);
    }
    // First-fit on a grid aligned to the tile size: a buddy layout without the tree.
    static std::optional<Owner> fit(Occupancy bits, std::uint32_t views, std::uint32_t size) {
        Owner owner;
        owner.views = views;
        owner.size = size;
        const std::uint32_t span = size / kMinShadowTile;
        for (std::uint32_t v = 0; v < views; ++v) {
            bool found = false;
            for (std::uint32_t y = 0; y + span <= kCells && !found; y += span)
                for (std::uint32_t x = 0; x + span <= kCells && !found; x += span) {
                    bool free = true;
                    for (std::uint32_t j = 0; j < span && free; ++j)
                        for (std::uint32_t i = 0; i < span && free; ++i) free = !bits[(y + j) * kCells + x + i];
                    if (!free) continue;
                    for (std::uint32_t j = 0; j < span; ++j)
                        for (std::uint32_t i = 0; i < span; ++i) bits[(y + j) * kCells + x + i] = true;
                    owner.cells[v] = {x, y};
                    found = true;
                }
            if (!found) return std::nullopt;
        }
        return owner;
    }
    static std::optional<Owner> fitDescending(const Occupancy& bits, const ShadowRequest& request) {
        for (std::uint32_t size = shadowTierSize(request.maxSize, request.contribution); size >= kMinShadowTile; size /= 2)
            if (auto owner = fit(bits, request.views, size)) return owner;
        return std::nullopt;
    }

    void place(std::uint64_t key, const ShadowRequest& request, double now) {
        auto owner = fitDescending(occupied_, request);
        if (!owner) {
            // Evict what nobody sees first, then lights worth less than half of this one (D13).
            std::vector<Owners::iterator> candidates;
            for (auto it = owners_.begin(); it != owners_.end(); ++it) {
                const bool unseen = it->second.lastSeen < now;
                if (!it->second.pinned && (unseen || it->second.reference * kShadowHysteresis < request.contribution))
                    candidates.push_back(it);
            }
            std::ranges::sort(candidates, [&](auto a, auto b) {
                const bool ua = a->second.lastSeen < now, ub = b->second.lastSeen < now;
                if (ua != ub) return ua;
                return ua ? a->second.lastSeen < b->second.lastSeen : a->second.reference < b->second.reference;
            });
            Occupancy trial = occupied_;
            std::size_t used = 0;
            for (; used < candidates.size() && !owner; ++used) {
                mark(trial, candidates[used]->second, false);
                owner = fitDescending(trial, request);
            }
            if (!owner) return;
            for (std::size_t i = 0; i < used; ++i) release(candidates[i]);
        }
        owner->reference = request.contribution;
        owner->lastSeen = now;
        owner->pinned = request.pinned;
        mark(occupied_, *owner, true);
        owners_[key] = *owner;
    }

    Owners owners_;
    Occupancy occupied_;
    std::uint32_t overflow_ = 0;
};

} // namespace apex::render
