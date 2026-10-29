#pragma once

#include "lighting/light.hpp"
#include "render/resources.hpp"

#include <cstddef>

namespace apex::render {

// std430 ABI shared with frame.inc and lighting.inc. Addresses are uvec2 in GLSL.
struct alignas(16) FrameBlock {
    VkDeviceAddress previews = 0;
    VkDeviceAddress lights = 0;
    std::uint32_t lightCount = 0;
    std::uint32_t flags = 0; // Bit 0: fullbright.
    std::uint32_t pad[2]{};
    Vec4 cameraRight{0.0f};
    Vec4 cameraUp{0.0f};
};
static_assert(sizeof(FrameBlock) == 64);
static_assert(offsetof(FrameBlock, cameraRight) == 32);

struct alignas(16) GpuLight {
    Vec4 originRadius;
    Vec4 intensityExponent;
    Vec4 directionInner;
    Vec4 attenuationOuter; // constant, linear, quadratic, outer cone cosine
    std::uint32_t kind = 0, directMode = 0, pad[2]{};
};
static_assert(sizeof(GpuLight) == 80);
static_assert(offsetof(GpuLight, kind) == 64);

inline GpuLight packLight(const lighting::Light& light, DVec3 camera) {
    return {Vec4(Vec3(light.origin - camera), static_cast<float>(light.radius)),
            Vec4(Vec3(light.intensity), static_cast<float>(light.exponent)),
            Vec4(Vec3(light.direction), static_cast<float>(light.stopdot)),
            Vec4(static_cast<float>(light.constant), static_cast<float>(light.linear),
                 static_cast<float>(light.quadratic), static_cast<float>(light.stopdot2)),
            static_cast<std::uint32_t>(light.kind), light.directMode, {}};
}

} // namespace apex::render
