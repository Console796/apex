#pragma once

#include "render/gpu_lighting.hpp"
#include "render/renderer.hpp"

namespace apex::render {

// 8x8 octahedral maps; each texel holds irradiance and first/second distance moments.
inline constexpr std::uint32_t kProbeMapSide = 8, kProbeRays = 128;
struct alignas(16) GpuIrradianceProbe {
    Vec4 irradiance[kProbeMapSide * kProbeMapSide]{}; // w: 0 uninitialised, 1 valid, -1 buried.
    Vec4 depth[kProbeMapSide * kProbeMapSide]{};
};
static_assert(sizeof(GpuIrradianceProbe) == 2048);
static_assert(offsetof(GpuIrradianceProbe, depth) == 1024);

// Owns one viewport's TLAS and ping-pong history. record() is outside rendering.
// Previous submissions using this volume must have completed before update/record/destruction.
class ProbeVolume {
  public:
    static Result<std::unique_ptr<ProbeVolume>> create(Context& context);
    ~ProbeVolume();
    Result<void> update(const DrawList& list, std::span<const GpuMaterial> materials);
    void record(VkCommandBuffer commands);
    void pack(FrameBlock& frame, DVec3 camera) const;
    void reset();
    VkDeviceAddress address() const;

  private:
    struct Impl;
    explicit ProbeVolume(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace apex::render
