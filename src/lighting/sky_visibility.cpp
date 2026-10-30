#include "lighting/sky_visibility.hpp"

#include "geometry/bvh.hpp"
#include "lighting/light.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace apex::lighting {
namespace {

constexpr std::array<DVec3, 6> kAxes{DVec3{1, 0, 0},  DVec3{-1, 0, 0}, DVec3{0, 1, 0},
                                     DVec3{0, -1, 0}, DVec3{0, 0, 1},  DVec3{0, 0, -1}};
constexpr double kMinHit = 1e-3; // hits closer than this are the surface the probe sits on

struct Hit {
    double t;
    bool backFace;
};

// Möller-Trumbore, both sides; reports whether the ray travels along the triangle's normal (a back face).
std::optional<Hit> intersect(const Ray& ray, const SkyTriangle& tri) {
    const DVec3 e1 = tri.b - tri.a, e2 = tri.c - tri.a;
    const DVec3 p = glm::cross(ray.dir, e2);
    const double det = glm::dot(e1, p);
    if (std::abs(det) < 1e-12) return std::nullopt;
    const double inv = 1.0 / det;
    const DVec3 s = ray.origin - tri.a;
    const double u = glm::dot(s, p) * inv;
    if (u < 0.0 || u > 1.0) return std::nullopt;
    const DVec3 q = glm::cross(s, e1);
    const double v = glm::dot(ray.dir, q) * inv;
    if (v < 0.0 || u + v > 1.0) return std::nullopt;
    const double t = glm::dot(e2, q) * inv;
    if (t < kMinHit) return std::nullopt;
    return Hit{t, glm::dot(glm::cross(e1, e2), ray.dir) > 0.0};
}

} // namespace

std::vector<DVec3> skyDirections(int rays) {
    // At least 16 upward rays, so every face axis has upward rays in its hemisphere (a zero open-sky
    // denominator would read as no sky).
    const int half = std::max(16, (rays + 1) / 2);
    const double golden = std::numbers::pi * (3.0 - std::sqrt(5.0));
    std::vector<DVec3> out;
    out.reserve(static_cast<std::size_t>(half) * 2);
    // Equal-area strata in z over the upper hemisphere, azimuth by the golden angle; mirrored below.
    for (int i = 0; i < half; ++i) {
        const double z = (i + 0.5) / half;
        const double r = std::sqrt(1.0 - z * z);
        const double phi = golden * i;
        out.push_back({r * std::cos(phi), r * std::sin(phi), z});
    }
    for (int i = 0; i < half; ++i) out.push_back({out[i].x, out[i].y, -out[i].z});
    return out;
}

double ambientCubeResponse(const AmbientCube& cube, DVec3 n) {
    const DVec3 sq = n * n;
    return sq.x * cube.value[n.x < 0 ? 1 : 0] + sq.y * cube.value[n.y < 0 ? 3 : 2] +
           sq.z * cube.value[n.z < 0 ? 5 : 4];
}

std::size_t SkyVisibilityGrid::validCount() const {
    return static_cast<std::size_t>(
        std::count_if(probes_.begin(), probes_.end(), [](const Probe& p) { return p.valid; }));
}

DVec3 SkyVisibilityGrid::probePosition(int x, int y, int z) const {
    return origin_ + cell_ * DVec3{x + 0.5, y + 0.5, z + 0.5};
}

double SkyVisibilityGrid::sample(DVec3 point, DVec3 normal) const {
    if (probes_.empty()) return 0.0;
    const DVec3 g = (point - origin_) / cell_ - DVec3{0.5};
    const int lim[3] = {dims_.x - 1, dims_.y - 1, dims_.z - 1};
    int i0[3];
    double f[3];
    for (int a = 0; a < 3; ++a) {
        const double c = std::clamp(g[a], 0.0, static_cast<double>(lim[a]));
        i0[a] = std::min(static_cast<int>(std::floor(c)), std::max(lim[a] - 1, 0));
        f[a] = lim[a] == 0 ? 0.0 : c - i0[a];
    }
    double sum = 0, weight = 0;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const double w = (dx ? f[0] : 1 - f[0]) * (dy ? f[1] : 1 - f[1]) * (dz ? f[2] : 1 - f[2]);
                if (w <= 0) continue;
                const int x = std::min(i0[0] + dx, lim[0]), y = std::min(i0[1] + dy, lim[1]),
                          z = std::min(i0[2] + dz, lim[2]);
                const Probe& p = probe(x, y, z);
                if (!p.valid) continue;
                sum += w * ambientCubeResponse(p.cube, normal);
                weight += w;
            }
    if (weight > 1e-9) return skyAmbientResponse(normal.z) * sum / weight;
    return 0.0; // No local air probe: match the GPU without leaking a distant room's sky.
}

SkyVisibilityGrid buildSkyVisibility(std::span<const SkyTriangle> triangles, const AABB& bounds,
                                     const SkyVisibilityOptions& options) {
    if (bounds.empty()) throw std::invalid_argument("buildSkyVisibility: empty bounds");
    SkyVisibilityGrid grid;
    const DVec3 extent = glm::max(bounds.max - bounds.min, DVec3{1.0});
    double spacing = std::max(options.spacing, 1.0);
    const std::size_t cap = std::max<std::size_t>(options.maxProbes, 1);
    int n[3];
    for (;;) {
        std::size_t total = 1;
        for (int a = 0; a < 3; ++a) {
            n[a] = std::max(1, static_cast<int>(std::ceil(extent[a] / spacing)));
            total *= static_cast<std::size_t>(n[a]);
        }
        if (total <= cap) break;
        spacing *= 1.1;
    }
    grid.dims_ = {n[0], n[1], n[2]};
    grid.origin_ = bounds.min;
    grid.cell_ = extent / DVec3{n[0], n[1], n[2]};
    grid.probes_.resize(static_cast<std::size_t>(n[0]) * n[1] * n[2]);

    std::vector<geometry::Bvh::Item> items;
    items.reserve(triangles.size());
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        AABB box;
        box.expand(triangles[i].a);
        box.expand(triangles[i].b);
        box.expand(triangles[i].c);
        items.push_back({static_cast<std::uint32_t>(i), box});
    }
    geometry::Bvh bvh;
    bvh.build(items);

    const std::vector<DVec3> dirs = skyDirections(options.rays);
    // Each face stores a visibility ratio: its response-weighted sky rays over what an open sky (every
    // upward ray) would give it. Sampling multiplies by the exact open-sky curve, so an open sky is exact
    // for every normal and the ambient cube only approximates partial occlusion. -Z sees no upward ray;
    // it takes the mean of the horizontal faces (its open-sky response is 0 anyway).
    std::array<std::vector<double>, 6> weights;
    std::array<double, 6> open{};
    for (int a = 0; a < 6; ++a) {
        weights[a].reserve(dirs.size());
        for (const DVec3& d : dirs) {
            weights[a].push_back(surfaceResponse(glm::dot(d, kAxes[a])));
            if (d.z > 0) open[a] += weights[a].back();
        }
    }
    // Far enough to find any hit: past the farthest point of the map and the geometry from any probe.
    AABB world = bounds;
    if (bvh.size() > 0) world.expand(bvh.bounds());
    const double reach = 2.0 * glm::length(world.max - world.min) + 1.0;

    for (int z = 0; z < n[2]; ++z)
        for (int y = 0; y < n[1]; ++y)
            for (int x = 0; x < n[0]; ++x) {
                if (options.cancelled && options.cancelled()) return {};
                const DVec3 origin = grid.probePosition(x, y, z);
                std::array<double, 6> sky{};
                std::size_t buried = 0;
                for (std::size_t r = 0; r < dirs.size(); ++r) {
                    const Ray ray{origin, dirs[r]};
                    double nearest = reach;
                    const SkyTriangle* hit = nullptr;
                    bool back = false;
                    bvh.raycast(ray, reach, [&](std::uint32_t id, double tMax) -> std::optional<double> {
                        const auto h = intersect(ray, triangles[id]);
                        if (!h || h->t >= std::min(tMax, nearest)) return std::nullopt;
                        nearest = h->t;
                        hit = &triangles[id];
                        back = h->backFace;
                        return h->t;
                    });
                    if (hit && back) ++buried;
                    const bool reachesSky = !hit || hit->sky;
                    if (reachesSky)
                        for (int a = 0; a < 6; ++a) sky[a] += weights[a][r];
                }
                auto& probe = grid.probes_[grid.index(x, y, z)];
                probe.valid = !(static_cast<double>(buried) > options.solidFraction * static_cast<double>(dirs.size()));
                if (probe.valid) {
                    for (int a = 0; a < 5; ++a)
                        probe.cube.value[a] = static_cast<float>(std::min(1.0, open[a] > 0 ? sky[a] / open[a] : 0.0));
                    probe.cube.value[5] = 0.25f * (probe.cube.value[0] + probe.cube.value[1] +
                                                   probe.cube.value[2] + probe.cube.value[3]);
                }
            }
    return grid;
}

} // namespace apex::lighting
