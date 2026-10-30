#pragma once

// Sky visibility for light_environment's ambient term (0.6 D16). A coarse probe grid, built on the CPU
// from world triangles, stores at each probe how much of the response-weighted hemisphere around each
// axis reaches the sky. Without it a sealed room glows with sky ambient the game's bake never gives it.
//
// A ray reaches the sky if its first hit is a sky triangle or it escapes the geometry (an unsealed
// editor map counts as open). The weighting is VRAD's (light.hpp skyAmbientResponse): each direction
// counts surfaceResponse(cos to the face axis). A face's value is the ratio of that weighted sky to what
// an open sky gives it: 1 under an open sky, 0 for a face that sees no sky.

#include "core/math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace apex::lighting {

// Triangles are wound counter-clockwise seen from the open side (the normal points into the air, as
// Source faces do). Rays are cast against both sides; the winding is only used to spot probes buried in
// solid space.
struct SkyTriangle {
    DVec3 a{0.0}, b{0.0}, c{0.0};
    bool sky = false; // the caller decides (tools/toolsskybox and friends); lighting hard-codes no material
};

// Ambient cube, in Source's face order: +X, -X, +Y, -Y, +Z, -Z.
struct AmbientCube {
    std::array<float, 6> value{};
};

struct SkyVisibilityOptions {
    double spacing = 128;           // preferred probe spacing, map units
    std::size_t maxProbes = 32768;  // spacing is coarsened until the grid fits
    int rays = 256;                 // sky directions per probe; rounded up to an even count, at least 32
    // A probe is buried in solid space when more than this fraction of its rays first hit a back face.
    double solidFraction = 0.5;
    // Polled once per probe; when it returns true the build stops and returns an empty grid.
    std::function<bool()> cancelled;
};

// The fixed stratified direction set: unit vectors, equal solid angle per ray, symmetric about the
// horizontal plane (every upward ray has a downward mirror with the same azimuth). Deterministic.
std::vector<DVec3> skyDirections(int rays);

// Blend an ambient cube by normal² components, as Source does. `normal` is unit.
double ambientCubeResponse(const AmbientCube& cube, DVec3 normal);

class SkyVisibilityGrid {
public:
    struct Dims {
        int x = 0, y = 0, z = 0;
    };
    struct Probe {
        AmbientCube cube;
        bool valid = false; // false: buried in solid space; sampling skips it
    };

    Dims dims() const { return dims_; }
    DVec3 origin() const { return origin_; } // minimum corner of the covered box
    DVec3 cellSize() const { return cell_; }
    std::size_t probeCount() const { return probes_.size(); }
    std::size_t validCount() const;
    DVec3 probePosition(int x, int y, int z) const;
    const Probe& probe(int x, int y, int z) const { return probes_[index(x, y, z)]; }

    // Gated sky ambient response for a surface at `point` facing the unit `normal`:
    // skyAmbientResponse(normal.z) times the visibility ratio, trilinear between the eight surrounding
    // probes, each blended by ambientCubeResponse. Invalid probes are skipped and the weights
    // renormalised; if all eight are invalid the response is 0 (also on the GPU).
    // Multiply the ambient light's intensity by this. Exact for every normal under a fully open sky.
    double sample(DVec3 point, DVec3 normal) const;

private:
    friend SkyVisibilityGrid buildSkyVisibility(std::span<const SkyTriangle>, const AABB&,
                                                const SkyVisibilityOptions&);
    std::size_t index(int x, int y, int z) const {
        return (static_cast<std::size_t>(z) * dims_.y + y) * dims_.x + x;
    }
    Dims dims_;
    DVec3 origin_{0.0}, cell_{1.0};
    std::vector<Probe> probes_;
};

// Builds the grid over `bounds` (the map bounds). Probes are independent of one another. Throws
// std::invalid_argument for empty bounds.
SkyVisibilityGrid buildSkyVisibility(std::span<const SkyTriangle> triangles, const AABB& bounds,
                                     const SkyVisibilityOptions& options = {});

} // namespace apex::lighting
