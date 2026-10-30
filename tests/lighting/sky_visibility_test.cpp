#include "lighting/sky_visibility.hpp"

#include "lighting/light.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace apex;
using namespace apex::lighting;

namespace {

using Tris = std::vector<SkyTriangle>;

// A quad centred at `c`, facing `n` (a unit axis), with half-extents `half`, wound so its normal is `n`.
void quad(Tris& out, DVec3 c, DVec3 n, double half, bool sky = false) {
    const DVec3 u = std::abs(n.z) < 0.5 ? glm::normalize(glm::cross(DVec3{0, 0, 1}, n)) : DVec3{1, 0, 0};
    const DVec3 v = glm::cross(n, u);
    const DVec3 hu = u * half, hv = v * half;
    out.push_back({c - hu - hv, c + hu - hv, c + hu + hv, sky});
    out.push_back({c - hu - hv, c + hu + hv, c - hu + hv, sky});
}

// Box room [-h,h]^3, every wall facing inward. `ceiling`: 0 solid, 1 sky.
Tris room(double h, bool skyCeiling = false) {
    Tris t;
    quad(t, {-h, 0, 0}, {1, 0, 0}, h);
    quad(t, {h, 0, 0}, {-1, 0, 0}, h);
    quad(t, {0, -h, 0}, {0, 1, 0}, h);
    quad(t, {0, h, 0}, {0, -1, 0}, h);
    quad(t, {0, 0, -h}, {0, 0, 1}, h);
    quad(t, {0, 0, h}, {0, 0, -1}, h, skyCeiling);
    return t;
}

AABB box(double h) { return {DVec3{-h}, DVec3{h}}; }

// Ceiling with a square hole of half-size `hole` (open to the sky), walls and floor solid.
Tris roomWithHole(double h, double hole) {
    Tris t = room(h);
    t.resize(t.size() - 2); // drop the ceiling
    const double q = (h - hole) / 2, mid = (h + hole) / 2;
    // four slabs around the hole, each a quad facing down
    const auto slab = [&](double cx, double cy, double hx, double hy) {
        const DVec3 a{cx - hx, cy - hy, h}, b{cx + hx, cy - hy, h}, c{cx + hx, cy + hy, h}, d{cx - hx, cy + hy, h};
        t.push_back({a, c, b}); // downward winding
        t.push_back({a, d, c});
    };
    slab(-mid, 0, q, h);
    slab(mid, 0, q, h);
    slab(0, -mid, hole, q);
    slab(0, mid, hole, q);
    return t;
}

double groundTruth(DVec3 n) { return skyAmbientResponse(n.z); }

} // namespace

TEST_CASE("sky visibility: open floor sees the whole sky above it") {
    Tris t;
    quad(t, {0, 0, 0}, {0, 0, 1}, 1e6);
    const auto grid = buildSkyVisibility(t, {{-512, -512, 0}, {512, 512, 512}});
    CHECK(grid.validCount() == grid.probeCount());
    for (int z = 0; z < grid.dims().z; ++z) {
        const auto& cube = grid.probe(1, 1, z).cube;
        for (float v : cube.value) CHECK(v == doctest::Approx(1.0).epsilon(1e-6));
    }
}

TEST_CASE("sky visibility: a sealed box is dark everywhere") {
    const auto grid = buildSkyVisibility(room(512), box(512));
    REQUIRE(grid.probeCount() > 1);
    CHECK(grid.validCount() == grid.probeCount());
    for (int z = 0; z < grid.dims().z; ++z)
        for (int y = 0; y < grid.dims().y; ++y)
            for (int x = 0; x < grid.dims().x; ++x)
                for (float v : grid.probe(x, y, z).cube.value) CHECK(v == 0.0f);
    CHECK(grid.sample({10, 20, 30}, {0, 0, 1}) == 0.0);
}

TEST_CASE("sky visibility: a sky ceiling lights the floor of a wide room") {
    // 8192 wide, 512 tall: only rays under ~6 degrees reach a wall, and they weigh almost nothing.
    Tris t;
    const double w = 4096, h = 256;
    quad(t, {0, 0, -h}, {0, 0, 1}, w);
    quad(t, {0, 0, h}, {0, 0, -1}, w, true);
    quad(t, {-w, 0, 0}, {1, 0, 0}, w);
    quad(t, {w, 0, 0}, {-1, 0, 0}, w);
    quad(t, {0, -w, 0}, {0, 1, 0}, w);
    quad(t, {0, w, 0}, {0, -1, 0}, w);
    const auto grid = buildSkyVisibility(t, {{-w, -w, -h}, {w, w, h}});
    const double up = grid.sample({0, 0, -h + 1}, {0, 0, 1});
    CHECK(up > 0.98);
    CHECK(up <= 1.0);
    CHECK(grid.sample({0, 0, 0}, {0, 0, -1}) == doctest::Approx(0.0).epsilon(1e-6));
    // In a plain box room the walls block the low rays, so the same probe sees less sky.
    const auto closed = buildSkyVisibility(room(512, true), box(512));
    const double mid = closed.sample({0, 0, 0}, {0, 0, 1});
    CHECK(mid > 0.5);
    CHECK(mid < 0.9);
}

TEST_CASE("sky visibility: a small hole lights less away from the hole") {
    const auto grid = buildSkyVisibility(roomWithHole(512, 256), box(512));
    const double under = grid.sample({0, 0, -448}, {0, 0, 1});
    const double corner = grid.sample({-448, -448, -448}, {0, 0, 1});
    CHECK(under > 0.05);
    CHECK(under < 0.6);
    CHECK(corner < under * 0.6);
    CHECK(corner >= 0.0);
}

TEST_CASE("sky visibility: sampling an open sky agrees with skyAmbientResponse") {
    Tris t;
    quad(t, {0, 0, 0}, {0, 0, 1}, 1e6);
    const auto grid = buildSkyVisibility(t, {{-512, -512, 0}, {512, 512, 512}});
    const DVec3 p{37, -91, 250}; // between probes
    // Exact on the axes and horizontally.
    for (const DVec3 n : {DVec3{0, 0, 1}, DVec3{0, 0, -1}, DVec3{1, 0, 0}, DVec3{0, -1, 0}})
        CHECK(grid.sample(p, n) == doctest::Approx(groundTruth(n)).epsilon(1e-6));
    // A horizontal normal in any azimuth blends two 0.5 faces.
    CHECK(grid.sample(p, glm::normalize(DVec3{1, 1, 0})) == doctest::Approx(0.5).epsilon(1e-6));
    // Diagonals too: the cube holds visibility ratios (all 1 here) and the open-sky curve is exact.
    for (int i = -20; i <= 20; ++i) {
        const double z = i / 20.0;
        const DVec3 n{std::sqrt(1 - z * z), 0, z};
        CHECK(grid.sample(p, n) == doctest::Approx(groundTruth(n)).epsilon(1e-6));
    }
}

TEST_CASE("sky visibility: buried probes are skipped") {
    // A slab of solid: the room's walls are far, the slab's faces point out of it, so probes inside the
    // slab see back faces.
    Tris t;
    const double h = 256;
    quad(t, {0, 0, h}, {0, 0, 1}, 1e5);   // slab top, facing up
    quad(t, {0, 0, -h}, {0, 0, -1}, 1e5); // slab bottom, facing down
    const auto grid = buildSkyVisibility(t, {{-256, -256, -256}, {256, 256, 256}});
    CHECK(grid.validCount() == 0);
    CHECK(grid.sample({0, 0, 0}, {0, 0, 1}) == 0.0);
    // Mixed: neighbours in air are renormalised; entirely buried neighbourhoods stay dark.
    Tris ground; // solid from z=-256 up to z=-100, open air above
    quad(ground, {0, 0, -100}, {0, 0, 1}, 1e5);
    quad(ground, {0, 0, -256}, {0, 0, -1}, 1e5);
    const auto open = buildSkyVisibility(ground, {{-256, -256, -256}, {256, 256, 256}});
    CHECK(open.validCount() > 0);
    CHECK(open.validCount() < open.probeCount());
    CHECK(open.sample({0, 0, -200}, {0, 0, 1}) == 0.0);
    CHECK(open.sample({0, 0, -100}, {0, 0, 1}) == doctest::Approx(1.0).epsilon(1e-6));
}

TEST_CASE("sky visibility: the grid fits the probe cap and results are deterministic") {
    SkyVisibilityOptions o;
    o.maxProbes = 20;
    o.rays = 64;
    const auto a = buildSkyVisibility(roomWithHole(1024, 128), box(1024), o);
    const auto b = buildSkyVisibility(roomWithHole(1024, 128), box(1024), o);
    CHECK(a.probeCount() <= 20);
    CHECK(a.cellSize().x > 128); // coarsened from the default spacing
    REQUIRE(a.probeCount() == b.probeCount());
    for (int z = 0; z < a.dims().z; ++z)
        for (int y = 0; y < a.dims().y; ++y)
            for (int x = 0; x < a.dims().x; ++x) {
                CHECK(a.probe(x, y, z).cube.value == b.probe(x, y, z).cube.value);
                CHECK(a.probe(x, y, z).valid == b.probe(x, y, z).valid);
            }
    CHECK(skyDirections(64) == skyDirections(64));
    CHECK(skyDirections(63).size() == 64);
    for (const DVec3& d : skyDirections(100)) CHECK(glm::length(d) == doctest::Approx(1.0));
}

TEST_CASE("sky visibility: build time over a 4096 box room" * doctest::skip(true)) {
    Tris t = room(2048);
    for (int i = 0; i < 150; ++i) quad(t, {-1900.0 + 25.0 * i, 600.0 * std::sin(i), -1900.0 + 20.0 * (i % 7)}, {0, 0, 1}, 30); // props
    const auto start = std::chrono::steady_clock::now();
    const auto grid = buildSkyVisibility(t, box(2048));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("sky grid: %zu probes, %zu triangles, 256 rays: %.2f s\n", grid.probeCount(), t.size(), seconds);
    CHECK(grid.probeCount() > 0);
}

TEST_CASE("sky visibility: the minimum ray count keeps an open sky exact on every face") {
    Tris t;
    quad(t, {0, 0, 0}, {0, 0, 1}, 1e6);
    SkyVisibilityOptions options;
    options.rays = 2;
    const auto grid = buildSkyVisibility(t, {{-512, -512, 0}, {512, 512, 512}}, options);
    for (const DVec3 n : {DVec3{1, 0, 0}, DVec3{-1, 0, 0}, DVec3{0, 1, 0}, DVec3{0, -1, 0}, DVec3{0, 0, 1}})
        CHECK(grid.sample({0, 0, 256}, n) == doctest::Approx(groundTruth(n)).epsilon(1e-6));
}

TEST_CASE("sky visibility: a cancelled build stops early and returns an empty grid") {
    SkyVisibilityOptions options;
    int polls = 0;
    options.cancelled = [&polls] { return ++polls > 3; };
    const auto grid = buildSkyVisibility(room(512), box(512), options);
    CHECK(grid.probeCount() == 0);
    CHECK(polls == 4);
    CHECK(grid.sample({0, 0, 0}, {0, 0, 1}) == 0.0);
}
