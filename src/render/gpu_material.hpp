#pragma once

#include "core/math.hpp"

#include <cstddef>
#include <cstdint>

namespace apex::render {

inline constexpr std::uint32_t kMaterialHasBump = 1u;
inline constexpr std::uint32_t kMaterialSsbump = 2u;
inline constexpr std::uint32_t kMaterialLinearBase = 4u;
inline constexpr std::uint32_t kMaterialTwoChannelNormal = 8u;
inline constexpr std::uint32_t kMaterialPbr = 16u;
inline constexpr std::uint32_t kMaterialPhong = 32u;
inline constexpr std::uint32_t kMaterialBaseAlphaPhongMask = 64u;
inline constexpr std::uint32_t kMaterialEnvmap = 128u;          // envmapSlot, or a probe with kMaterialEnvmapProbe.
inline constexpr std::uint32_t kMaterialEnvmapProbe = 256u;     // $envmap env_cubemap: the nearest probe's cube.
inline constexpr std::uint32_t kMaterialBaseAlphaEnvmapMask = 512u;
inline constexpr std::uint32_t kMaterialNormalAlphaEnvmapMask = 1024u;
inline constexpr std::uint32_t kMaterialSelfIllum = 2048u;
inline constexpr std::uint32_t kMaterialLocalRefract = 4096u; // Refract with $localrefract: unlit, offset base.
inline constexpr std::uint32_t kMaterialParallax = 8192u;     // PBR $parallax: height in the bump alpha.
inline constexpr std::uint32_t kNoTexture = 0xFFFFFFFFu;
inline constexpr std::uint32_t kDefaultEnvmapCube = 0u; // TextureHeap's kDefaultCubeSlot.

// std430, indexed by the base texture slot. Zero records preserve legacy shading.
struct alignas(16) GpuMaterial {
    std::uint32_t baseSlot = 0;
    std::uint32_t bumpSlot = kNoTexture;
    std::uint32_t flags = 0;
    std::uint32_t selfIllumMaskSlot = kNoTexture; // 2D; R masks, else base alpha.
    std::uint32_t mraoSlot = kNoTexture;
    std::uint32_t phongExponentSlot = kNoTexture;
    std::uint32_t envmapSlot = kDefaultEnvmapCube; // Cube slot (TextureHeap binding 2).
    std::uint32_t envmapMaskSlot = kNoTexture;     // 2D; RGB masks.
    Vec4 mraoScale{1, 1, 1, 0};
    Vec4 phongParams{5, 1, 0, 0}; // exponent, boost; zw reserved
    Vec4 phongFresnel{0, 0.5f, 1, 0};
    Vec4 phongTint{1, 1, 1, 0};
    Vec4 envmapTint{1, 1, 1, 0}; // w = $envmaplightscale
    Vec4 selfIllumTint{1, 1, 1, 0};
    Vec4 envmapParams{0, 1, 0, 0}; // $envmapcontrast, $envmapsaturation; zw reserved
    Vec4 refractTint{1, 1, 1, 0};
    Vec4 refractParams{0, 0, 0, 0}; // $refractamount, $localrefractdepth; zw reserved
    Vec4 parallaxParams{0, 0, 1, 0}; // $parallaxdepth, $parallaxcenter, $parallaxscale; w reserved
};
static_assert(sizeof(GpuMaterial) == 192);
static_assert(alignof(GpuMaterial) == 16);
static_assert(offsetof(GpuMaterial, mraoSlot) == 16);
static_assert(offsetof(GpuMaterial, phongExponentSlot) == 20);
static_assert(offsetof(GpuMaterial, mraoScale) == 32);
static_assert(offsetof(GpuMaterial, phongParams) == 48);
static_assert(offsetof(GpuMaterial, phongFresnel) == 64);
static_assert(offsetof(GpuMaterial, phongTint) == 80);
static_assert(offsetof(GpuMaterial, flags) == 8);
static_assert(offsetof(GpuMaterial, selfIllumMaskSlot) == 12);
static_assert(offsetof(GpuMaterial, envmapSlot) == 24);
static_assert(offsetof(GpuMaterial, envmapMaskSlot) == 28);
static_assert(offsetof(GpuMaterial, envmapTint) == 96);
static_assert(offsetof(GpuMaterial, selfIllumTint) == 112);
static_assert(offsetof(GpuMaterial, envmapParams) == 128);
static_assert(offsetof(GpuMaterial, refractTint) == 144);
static_assert(offsetof(GpuMaterial, refractParams) == 160);
static_assert(offsetof(GpuMaterial, parallaxParams) == 176);

} // namespace apex::render
