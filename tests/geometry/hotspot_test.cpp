// Plan 0.8 D13 (0.8.7): fitting a face to a hotspot rect, against fixed rect sets: aspect and size
// choice, quarter turns, the rotate, reflect and alt flags, and seeded choice among equals.
#include "geometry/hotspot.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <set>

using namespace apex;
using namespace apex::geometry;

namespace {

// A floor face [0, w] x [0, h] at z = 0, facing up, with Hammer's top-view axes.
std::vector<DVec3> floorFace(double w, double h) { return {{0, 0, 0}, {w, 0, 0}, {w, h, 0}, {0, h, 0}}; }
FaceUv topAxes() { return {{1, 0, 0}, {0, -1, 0}, 0, 0, 0.25, 0.25}; }
constexpr DVec3 kUp{0, 0, 1};

struct Box { double u0, u1, v0, v1; };
// The texel bounds of the face's points under `uv`.
Box texels(const FaceUv& uv, const std::vector<DVec3>& points) {
    Box box{1e18, -1e18, 1e18, -1e18};
    for (const auto& p : points) {
        const double u = glm::dot(p, uv.uaxis) / uv.scaleU + uv.shiftU, v = glm::dot(p, uv.vaxis) / uv.scaleV + uv.shiftV;
        box = {std::min(box.u0, u), std::max(box.u1, u), std::min(box.v0, v), std::max(box.v1, v)};
    }
    return box;
}
void checkFills(const HotspotFit& fit, const HotspotRect& rect, const std::vector<DVec3>& points) {
    const auto box = texels(fit.uv, points);
    CHECK(box.u0 == doctest::Approx(rect.x1));
    CHECK(box.u1 == doctest::Approx(rect.x2));
    CHECK(box.v0 == doctest::Approx(rect.y1));
    CHECK(box.v1 == doctest::Approx(rect.y2));
}
HotspotFit fit(const std::vector<DVec3>& points, const std::vector<HotspotRect>& rects, HotspotFitOptions options = {}) {
    auto result = fitHotspot(topAxes(), kUp, points, rects, options);
    REQUIRE(result);
    return *result;
}

} // namespace

TEST_CASE("hotspot fit picks the rect nearest the face's aspect, then its size") {
    const std::vector<HotspotRect> rects{{0, 0, 512, 128}, {0, 128, 256, 256}, {256, 128, 512, 384}, {0, 384, 256, 448}};
    // 128 x 32 units is 512 x 128 texels at 0.25: rect 0 exactly.
    const auto strip = floorFace(128, 32);
    auto result = fit(strip, rects);
    CHECK(result.rect == 0);
    CHECK(result.quarterTurns == 0);
    CHECK_FALSE(result.reflected);
    checkFills(result, rects[0], strip);
    CHECK(result.uv.scaleU == doctest::Approx(0.25));
    // Half that size is 4:1 too, and rect 3 (256 x 64) is its size.
    const auto small = floorFace(64, 16);
    result = fit(small, rects);
    CHECK(result.rect == 3);
    checkFills(result, rects[3], small);
    // A square face takes the square rect, stretched to fill it whatever its size.
    const auto square = floorFace(40, 40);
    result = fit(square, rects);
    CHECK(result.rect == 2);
    checkFills(result, rects[2], square);
    CHECK(result.uv.scaleU == doctest::Approx(40.0 / 256));
}

TEST_CASE("hotspot fit turns a strip a quarter to run along a tall face, but never at random") {
    const std::vector<HotspotRect> rects{{0, 0, 512, 128}};
    const auto pillar = floorFace(32, 128);
    const auto result = fit(pillar, rects);
    CHECK(result.quarterTurns == 1);
    checkFills(result, rects[0], pillar);
    // The rect's long side runs along the face's long side.
    CHECK(std::abs(glm::dot(result.uv.uaxis, DVec3{0, 1, 0})) == doctest::Approx(1.0));
    // A square rect without the rotate flag keeps the face's frame for every seed.
    const std::vector<HotspotRect> square{{0, 0, 128, 128}};
    for (std::uint64_t seed = 0; seed < 32; ++seed) {
        const auto r = fit(floorFace(32, 32), square, {.seed = seed});
        CHECK(r.quarterTurns == 0);
        CHECK(r.uv.uaxis == DVec3{1, 0, 0});
    }
}

TEST_CASE("hotspot fit: the rotate and reflect flags choose by seed, and a seed repeats") {
    const auto face = floorFace(32, 32);
    const std::vector<HotspotRect> rotate{{0, 0, 128, 128, true}};
    std::set<int> turns;
    for (std::uint64_t seed = 0; seed < 64; ++seed) {
        const auto r = fit(face, rotate, {.seed = seed});
        turns.insert(r.quarterTurns);
        checkFills(r, rotate[0], face);
        const auto again = fit(face, rotate, {.seed = seed});
        CHECK(again.quarterTurns == r.quarterTurns);
        CHECK(again.uv.uaxis == r.uv.uaxis);
    }
    CHECK(turns == std::set{0, 1, 2, 3});
    // A non-square rotate rect turns only by halves on a face of its aspect.
    const std::vector<HotspotRect> strip{{0, 0, 512, 128, true}};
    for (std::uint64_t seed = 0; seed < 16; ++seed) CHECK(fit(floorFace(128, 32), strip, {.seed = seed}).quarterTurns % 2 == 0);

    const std::vector<HotspotRect> reflect{{0, 0, 128, 128, false, true}};
    std::set<bool> flips;
    for (std::uint64_t seed = 0; seed < 32; ++seed) {
        const auto r = fit(face, reflect, {.seed = seed});
        flips.insert(r.reflected);
        checkFills(r, reflect[0], face);
        // Flipped, the face's +x edge is at the rect's left.
        const double u = glm::dot(DVec3{32, 0, 0}, r.uv.uaxis) / r.uv.scaleU + r.uv.shiftU;
        CHECK(u == doctest::Approx(r.reflected ? 0.0 : 128.0));
    }
    CHECK(flips == std::set{false, true});
}

TEST_CASE("hotspot fit chooses among rects of the same size by seed") {
    const std::vector<HotspotRect> rects{{0, 0, 128, 128}, {128, 0, 256, 128}, {256, 0, 384, 128}, {0, 128, 512, 256}};
    std::set<std::size_t> chosen;
    for (std::uint64_t seed = 0; seed < 64; ++seed) chosen.insert(fit(floorFace(32, 32), rects, {.seed = seed}).rect);
    CHECK(chosen == std::set<std::size_t>{0, 1, 2});
}

TEST_CASE("hotspot fit: the alternate group is used only when asked, and then alone") {
    const std::vector<HotspotRect> rects{{0, 0, 512, 128}, {0, 128, 512, 256, false, false, true}, {0, 256, 128, 384, false, false, true}};
    std::set<std::size_t> normal, alternate;
    for (std::uint64_t seed = 0; seed < 32; ++seed) {
        normal.insert(fit(floorFace(128, 32), rects, {.seed = seed}).rect);
        alternate.insert(fit(floorFace(128, 32), rects, {.alternate = true, .seed = seed}).rect);
    }
    CHECK(normal == std::set<std::size_t>{0});
    CHECK(alternate == std::set<std::size_t>{1});
    CHECK(fit(floorFace(32, 32), rects, {.alternate = true}).rect == 2);
    const std::vector<HotspotRect> plain{{0, 0, 64, 64}};
    CHECK_FALSE(fitHotspot(topAxes(), kUp, floorFace(32, 32), plain, {.alternate = true}));
}

TEST_CASE("hotspot fit works on a sloped face and refuses what it cannot fit") {
    // A 45-degree ramp, 128 along x and 64 * sqrt(2) up the slope.
    const DVec3 normal = glm::normalize(DVec3{0, -1, 1});
    const std::vector<DVec3> ramp{{0, 0, 0}, {128, 0, 0}, {128, 64, 64}, {0, 64, 64}};
    const std::vector<HotspotRect> rects{{0, 0, 512, 362}, {0, 0, 512, 128}};
    const auto result = fitHotspot(topAxes(), normal, ramp, rects, {});
    REQUIRE(result);
    CHECK(result->rect == 0);
    CHECK(glm::dot(result->uv.uaxis, normal) == doctest::Approx(0.0));
    CHECK(glm::dot(result->uv.vaxis, normal) == doctest::Approx(0.0));
    checkFills(*result, rects[0], ramp);

    CHECK_FALSE(fitHotspot(topAxes(), kUp, floorFace(32, 32), {}, {}));
    const std::vector<DVec3> line{{0, 0, 0}, {32, 0, 0}, {64, 0, 0}};
    CHECK_FALSE(fitHotspot(topAxes(), kUp, line, rects, {}));
    CHECK_FALSE(fitHotspot(topAxes(), kUp, floorFace(32, 32), rects, {.texelScale = 0}));
}
