#pragma once

#include "core/math.hpp"
#include "core/result.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace apex::lighting {

// Immutable draw-local positions and triangle indices retained at upload, never GPU readback.
struct TriangleMesh {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> materials; // Optional per-vertex source material IDs; interpreted by the caller.
};

using RayTransform = std::array<std::array<float, 4>, 3>; // Row-major object-to-world.

struct RayHit {
    float distance;
    std::uint32_t customIndex;
    std::uint32_t primitive;
};

class CpuBlas {
  public:
    static Result<std::shared_ptr<CpuBlas>> build(std::span<const std::array<float, 3>> positions,
                                                std::span<const std::uint32_t> indices);
    ~CpuBlas();
    CpuBlas(const CpuBlas&) = delete;
    CpuBlas& operator=(const CpuBlas&) = delete;

  private:
    friend class CpuAccel;
    struct Impl;
    explicit CpuBlas(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct CpuInstance {
    std::shared_ptr<CpuBlas> blas;
    RayTransform transform;
    std::uint32_t customIndex = 0;
};

// Embree instances over immutable triangle scenes. Calls that modify a scene are exclusive;
// queries may run concurrently after build/update returns. Rays use [0, tmax]; distance is
// the ray parameter (world units for unit directions), as in Vulkan ray queries.
class CpuAccel {
  public:
    static Result<std::unique_ptr<CpuAccel>> build(std::span<const CpuInstance> instances);
    Result<void> updateInstances(std::span<const CpuInstance> instances);
    ~CpuAccel();
    CpuAccel(const CpuAccel&) = delete;
    CpuAccel& operator=(const CpuAccel&) = delete;

    bool occluded(Vec3 origin, Vec3 direction, float tmax) const;
    std::optional<RayHit> closest(Vec3 origin, Vec3 direction, float tmax) const;

  private:
    struct Impl;
    explicit CpuAccel(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace apex::lighting
