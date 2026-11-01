#pragma once

#include "lighting/light.hpp"
#include "lighting/sky_visibility.hpp"
#include "render/resources.hpp"
#include "render/gpu_material.hpp"

#include <cstddef>

namespace apex::render {

// std430 ABI shared with frame.inc and lighting.inc. Addresses are uvec2 in GLSL.
struct alignas(16) FrameBlock {
    VkDeviceAddress previews = 0;
    VkDeviceAddress lights = 0;
    std::uint32_t lightCount = 0;
    std::uint32_t flags = 0; // Bit 0: fullbright; bit 1: cubemap capture.
    std::uint32_t pad[2]{};
    Vec4 cameraRight{0.0f};
    Vec4 cameraUp{0.0f};
    VkDeviceAddress skyProbes = 0;
    std::uint32_t skyPad[2]{};
    glm::uvec4 skyDims{0}; // xyz dimensions; w reserved.
    Vec4 skyOrigin{0.0f}; // Camera-relative, in map units.
    Vec4 skyCellSize{1.0f};
    VkDeviceAddress materials = 0;
    std::uint32_t materialCount = 0, materialPad = 0;
    VkDeviceAddress cubemaps = 0; // GpuCubemapProbe[cubemapCount]
    std::uint32_t cubemapCount = 0, cubemapPad = 0;
    VkDeviceAddress clusters = 0;
    VkDeviceAddress clusterStats = 0;
    glm::uvec4 clusterDims{0}; // xyz dimensions; w global light count (prefix of lights).
    Mat4 clusterProjection{1.0f}; // Camera-relative world to clip.
    Vec4 fogColour{0.0f};
    Vec4 fogParams{0.0f}; // start, end, max density, enabled.
    Mat4 skyInverseView{1.0f};
    glm::uvec4 skyFaces[2]{};
    Vec4 skyTransform{0.0f}; // xyz translation, w scale.
    VkDeviceAddress shadows = 0;
    std::uint32_t shadowAtlas = 0, shadowCount = 0;
};
static_assert(sizeof(FrameBlock) == 416);
static_assert(offsetof(FrameBlock, shadows) == 400);
static_assert(offsetof(FrameBlock, shadowAtlas) == 408);
static_assert(offsetof(FrameBlock, clusters) == 160);
static_assert(offsetof(FrameBlock, clusterStats) == 168);
static_assert(offsetof(FrameBlock, clusterDims) == 176);
static_assert(offsetof(FrameBlock, clusterProjection) == 192);
static_assert(offsetof(FrameBlock, fogColour) == 256);
static_assert(offsetof(FrameBlock, skyInverseView) == 288);
static_assert(offsetof(FrameBlock, skyTransform) == 384);
static_assert(offsetof(FrameBlock, cubemaps) == 144);
static_assert(offsetof(FrameBlock, cubemapCount) == 152);
static_assert(offsetof(FrameBlock, materials) == 128);
static_assert(offsetof(FrameBlock, materialCount) == 136);
static_assert(offsetof(FrameBlock, previews) == 0);
static_assert(offsetof(FrameBlock, lights) == 8);
static_assert(offsetof(FrameBlock, lightCount) == 16);
static_assert(offsetof(FrameBlock, flags) == 20);
static_assert(offsetof(FrameBlock, cameraRight) == 32);
static_assert(offsetof(FrameBlock, cameraUp) == 48);
static_assert(offsetof(FrameBlock, skyProbes) == 64);
static_assert(offsetof(FrameBlock, skyDims) == 80);
static_assert(offsetof(FrameBlock, skyOrigin) == 96);
static_assert(offsetof(FrameBlock, skyCellSize) == 112);

// std430 scalar array plus uint: 28-byte stride, 4-byte alignment.
struct GpuSkyProbe {
    float value[6]{};
    std::uint32_t valid = 0;
};
static_assert(sizeof(GpuSkyProbe) == 28);
static_assert(alignof(GpuSkyProbe) == 4);
static_assert(offsetof(GpuSkyProbe, valid) == 24);

inline GpuSkyProbe packSkyProbe(const lighting::SkyVisibilityGrid::Probe& probe) {
    GpuSkyProbe packed;
    for (int i = 0; i < 6; ++i) packed.value[i] = probe.cube.value[i];
    packed.valid = probe.valid ? 1u : 0u;
    return packed;
}

inline void packSkyGrid(FrameBlock& frame, const lighting::SkyVisibilityGrid& grid,
                        VkDeviceAddress probes, DVec3 camera) {
    const auto dims = grid.dims();
    frame.skyProbes = probes;
    frame.skyDims = {dims.x, dims.y, dims.z, 0};
    frame.skyOrigin = Vec4(Vec3(grid.origin() - camera), 0);
    frame.skyCellSize = Vec4(Vec3(grid.cellSize()), 0);
}

// std430 vec3 + uint + mat4: 80-byte stride, 16-byte alignment.
struct alignas(16) GpuCubemapProbe {
    float position[3]{}; // Camera-relative.
    std::uint32_t cube = 0;
    Mat4 worldToBox{0.0f}; // Zero disables correction.
};
static_assert(sizeof(GpuCubemapProbe) == 80);
static_assert(offsetof(GpuCubemapProbe, cube) == 12);

struct alignas(16) GpuLight {
    Vec4 originRadius;
    Vec4 intensityExponent;
    Vec4 directionInner;
    Vec4 attenuationOuter; // constant, linear, quadratic, outer cone cosine
    std::uint32_t kind = 0, directMode = 0, specularMode = 0, indirectMode = 0;
    // 0 = unshadowed; otherwise first tile index + 1 and the number of views: 1 spot, 6 point, 2-3 cascades (D14).
    std::uint32_t shadow = 0, shadowViews = 0, shadowPad[2]{};
};
static_assert(sizeof(GpuLight) == 96);
static_assert(offsetof(GpuLight, shadow) == 80);
static_assert(offsetof(GpuLight, kind) == 64);
static_assert(offsetof(GpuLight, directMode) == 68);
static_assert(offsetof(GpuLight, specularMode) == 72);
static_assert(offsetof(GpuLight, indirectMode) == 76);

inline GpuLight packLight(const lighting::Light& light, DVec3 camera) {
    return {Vec4(Vec3(light.origin - camera), static_cast<float>(light.radius)),
            Vec4(Vec3(light.intensity), static_cast<float>(light.exponent)),
            Vec4(Vec3(light.direction), static_cast<float>(light.stopdot)),
            Vec4(static_cast<float>(light.constant), static_cast<float>(light.linear),
                 static_cast<float>(light.quadratic), static_cast<float>(light.stopdot2)),
            static_cast<std::uint32_t>(light.kind), light.directMode, light.specularMode, light.indirectMode};
}

} // namespace apex::render
