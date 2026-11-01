#include "render/shadow_alloc.hpp"
#include "render/view_math.hpp"

#include <doctest/doctest.h>

#include <set>

using namespace apex;
using namespace apex::render;

namespace {

ShadowRequest request(std::uint64_t key, double contribution, std::uint32_t views = 1, std::uint32_t maxSize = 1024,
                      bool inView = true) {
    ShadowRequest r;
    r.key = key; r.views = views; r.maxSize = maxSize; r.contribution = contribution; r.inView = inView;
    return r;
}

bool overlap(const ShadowRect& a, const ShadowRect& b) {
    return a.x < b.x + b.size && b.x < a.x + a.size && a.y < b.y + b.size && b.y < a.y + a.size;
}

void checkDisjoint(const std::vector<ShadowGrant>& grants) {
    std::vector<ShadowRect> all;
    for (const auto& g : grants)
        for (std::uint32_t v = 0; v < g.views; ++v) {
            const auto& r = g.rects[v];
            CHECK(r.x + r.size <= kShadowAtlasSize);
            CHECK(r.y + r.size <= kShadowAtlasSize);
            CHECK(r.x % r.size == 0);
            CHECK(r.y % r.size == 0);
            all.push_back(r);
        }
    for (std::size_t i = 0; i < all.size(); ++i)
        for (std::size_t j = i + 1; j < all.size(); ++j) CHECK_FALSE(overlap(all[i], all[j]));
}

} // namespace

TEST_CASE("shadow: tiers halve the tile for every factor of four below full contribution") {
    CHECK(shadowTierSize(1024, 1.0) == 1024);
    CHECK(shadowTierSize(1024, 0.3) == 512);
    CHECK(shadowTierSize(1024, 0.05) == 256);
    CHECK(shadowTierSize(1024, 1e-6) == kMinShadowTile);
    CHECK(shadowTierSize(512, 1e-6) == kMinShadowTile);
}

TEST_CASE("shadow: a scripted camera sweep never changes a tile's owner or place while contributions hold steady") {
    ShadowAllocator allocator;
    std::vector<ShadowRequest> requests;
    // Eight spots and four points (224 of the atlas's 256 cells), all at steady contributions of 1-2.
    for (std::uint64_t i = 1; i <= 12; ++i) requests.push_back(request(i, 1.0 + double(i % 5) * 0.25, i % 3 == 0 ? 6 : 1, i % 3 == 0 ? 512 : 1024));
    const auto base = requests;
    auto first = allocator.update(requests, 0.0);
    checkDisjoint(first);
    for (const auto& g : first) REQUIRE(g.shadowed);
    CHECK(allocator.overflow() == 0);
    for (int frame = 1; frame < 600; ++frame) {
        // The camera moves, so contributions wobble by up to +-30%; none crosses the 2x hysteresis.
        for (std::size_t i = 0; i < requests.size(); ++i)
            requests[i].contribution = base[i].contribution * (1.0 + 0.3 * std::sin(double(frame) * 0.1 + double(i)));
        const auto grants = allocator.update(requests, double(frame) / 60.0);
        for (std::size_t i = 0; i < requests.size(); ++i) {
            CHECK(grants[i].shadowed == first[i].shadowed);
            for (std::uint32_t v = 0; v < grants[i].views; ++v) {
                CHECK(grants[i].rects[v].x == first[i].rects[v].x);
                CHECK(grants[i].rects[v].y == first[i].rects[v].y);
                CHECK(grants[i].rects[v].size == first[i].rects[v].size);
            }
        }
    }
}

TEST_CASE("shadow: a light re-tiers only after its contribution moves by more than twice") {
    ShadowAllocator allocator;
    std::vector<ShadowRequest> requests{request(7, 1.0)};
    const auto base = allocator.update(requests, 0.0)[0];
    REQUIRE(base.shadowed);
    CHECK(base.rects[0].size == 1024);
    requests[0].contribution = 0.55; // Within 2x of the reference: the tile stays 1024 although the tier is now 512.
    CHECK(allocator.update(requests, 0.1)[0].rects[0].size == 1024);
    requests[0].contribution = 0.2; // Beyond 2x: re-tiers, to 256.
    const auto smaller = allocator.update(requests, 0.2)[0];
    CHECK(smaller.rects[0].size == 256);
    requests[0].contribution = 0.35; // Within 2x of the new reference: stays.
    CHECK(allocator.update(requests, 0.3)[0].rects[0].size == 256);
}

TEST_CASE("shadow: an out-of-view light keeps its tile for one second") {
    ShadowAllocator allocator;
    std::vector<ShadowRequest> requests{request(1, 1.0), request(2, 1.0)};
    const auto base = allocator.update(requests, 0.0);
    requests[1].inView = false;
    const auto gone = allocator.update(requests, 0.5);
    CHECK_FALSE(gone[1].shadowed);
    CHECK(allocator.ownerCount() == 2);
    requests[1].inView = true;
    const auto back = allocator.update(requests, 0.9);
    REQUIRE(back[1].shadowed);
    CHECK(back[1].rects[0].x == base[1].rects[0].x);
    CHECK(back[1].rects[0].y == base[1].rects[0].y);
    requests[1].inView = false;
    allocator.update(requests, 1.0);
    allocator.update(requests, 2.5); // More than a second out of view: released.
    CHECK(allocator.ownerCount() == 1);
    CHECK(allocator.overflow() == 0);
}

TEST_CASE("shadow: overflow counts in-view lights that do not fit and extras stay unshadowed") {
    ShadowAllocator allocator;
    std::vector<ShadowRequest> requests;
    // 16 full 1024 tiles fill the 4096 atlas exactly; every light is equally weighty, so nothing is evictable.
    for (std::uint64_t i = 1; i <= 20; ++i) requests.push_back(request(i, 1.0));
    const auto grants = allocator.update(requests, 0.0);
    unsigned shadowed = 0;
    for (const auto& g : grants) shadowed += g.shadowed ? 1 : 0;
    CHECK(shadowed >= 16);
    CHECK(allocator.overflow() + shadowed == 20);
    CHECK(allocator.overflow() == 20 - shadowed);
    checkDisjoint(grants);
    // Lights outside the view are never overflow.
    for (auto& r : requests) r.inView = false;
    allocator.update(requests, 0.1);
    CHECK(allocator.overflow() == 0);
}

TEST_CASE("shadow: placement is stable across frames and an important newcomer evicts a half-as-weighty owner") {
    ShadowAllocator allocator;
    std::vector<ShadowRequest> requests;
    for (std::uint64_t i = 1; i <= 16; ++i) requests.push_back(request(i, 1.0));
    allocator.update(requests, 0.0);
    CHECK(allocator.overflow() == 0);
    requests.push_back(request(99, 1.0));
    allocator.update(requests, 0.1);
    CHECK(allocator.overflow() == 1); // Equal weight does not evict.
    requests.back().contribution = 8.0;
    const auto grants = allocator.update(requests, 0.2);
    CHECK(grants.back().shadowed);
    CHECK(allocator.overflow() == 1); // The evicted owner now renders unshadowed and is counted.
    CHECK(std::ranges::count_if(grants, [](const ShadowGrant& g) { return !g.shadowed; }) == 1);
    checkDisjoint(grants);
}

TEST_CASE("shadow: the sun is pinned and takes its cascades before anything else") {
    ShadowAllocator allocator;
    std::vector<ShadowRequest> requests;
    for (std::uint64_t i = 1; i <= 30; ++i) requests.push_back(request(i, 16.0));
    ShadowRequest sun = request(1000, 1e9, kSunCascades, kMaxShadowTile);
    sun.pinned = true;
    requests.push_back(sun);
    const auto grants = allocator.update(requests, 0.0);
    REQUIRE(grants.back().shadowed);
    CHECK(grants.back().views == kSunCascades);
    checkDisjoint(grants);
}

TEST_CASE("shadow: requests are made for shadowable lights only") {
    const Mat4 vp = reversedZPerspective(glm::radians(90.0), 1.0, 1) * rotationOnlyView(angleVectors(DVec3{0, 0, 0}));
    lighting::Light point;
    point.kind = lighting::LightKind::Point;
    point.origin = {100, 0, 0};
    point.intensity = DVec3(1); point.constant = 1;
    const auto r = shadowRequest(point, DVec3(0.0), vp, 200);
    REQUIRE(r);
    CHECK(r->views == 6);
    CHECK(r->inView);
    CHECK_FALSE(shadowRequest(point, DVec3(0.0), vp, 0));
    point.directMode = 0; point.specularMode = 0;
    CHECK_FALSE(shadowRequest(point, DVec3(0.0), vp, 200));
    lighting::Light behind;
    behind.kind = lighting::LightKind::Point;
    behind.origin = {-3000, 0, 0};
    behind.intensity = DVec3(1); behind.constant = 1;
    CHECK_FALSE(shadowRequest(behind, DVec3(0.0), vp, 100)->inView);
    lighting::Light spot = behind;
    spot.kind = lighting::LightKind::Spot;
    spot.origin = {100, 0, 0};
    spot.direction = {-1, 0, 0};
    spot.stopdot = 0.9; spot.stopdot2 = 0.8; // Narrow: one 2D tile.
    CHECK(shadowRequest(spot, DVec3(0.0), vp, 200)->views == 1);
    spot.stopdot2 = 0.1; // Wider than 120 degrees: a cube.
    CHECK(shadowRequest(spot, DVec3(0.0), vp, 200)->views == 6);
    lighting::Light ambient;
    ambient.kind = lighting::LightKind::Ambient;
    CHECK_FALSE(shadowRequest(ambient, DVec3(0.0), vp, 200));
}
