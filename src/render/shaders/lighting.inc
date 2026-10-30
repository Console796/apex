#ifndef APEX_LIGHTING_INC
#define APEX_LIGHTING_INC
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

#include "frame.inc"

// GpuLight in gpu_lighting.hpp; std430, 80-byte stride.
struct Light {
    vec4 originRadius;
    vec4 intensityExponent;
    vec4 directionInner;
    vec4 attenuationOuter;
    uvec4 kindMode;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LightBuffer {
    Light lights[];
};

float surfaceResponse(float cosine) { return cosine > 0.0 ? 0.5 * cosine * (1.0 + cosine) : 0.0; }

// GpuSkyProbe in gpu_lighting.hpp; std430, 28-byte stride.
struct SkyProbe {
    float value[6];
    uint valid;
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer SkyProbeBuffer {
    SkyProbe probes[];
};

// asin's GLSL accuracy is driver-dependent (llvmpipe misses the CPU parity tolerance).
// Taylor series through x^17 on [-0.5, 0.5], error < 2.4e-8 before float rounding.
float skyAsinSmall(float x) {
    float q = x * x;
    return x + x * q * (1.0 / 6.0 + q * (3.0 / 40.0 + q * (5.0 / 112.0 +
        q * (35.0 / 1152.0 + q * (63.0 / 2816.0 + q * (231.0 / 13312.0 +
        q * (143.0 / 10240.0 + q * (6435.0 / 557056.0))))))));
}

float skyAmbientResponse(float normalZ) {
    float z = clamp(normalZ, -1.0, 1.0);
    float a = abs(z);
    const float k = 2.0 / (5.0 * 3.141592653589793);
    if (a > 0.5) {
        // asin(a) = pi/2 - 2 asin(sqrt((1-a)/2)). Evaluate the low side without
        // subtracting pi/2, preserving the small response of nearly downward normals.
        float t = sqrt((1.0 - a) * 0.5);
        float low = 0.3 * (1.0 - a) + k * (2.0 * skyAsinSmall(t) - a * sqrt((1.0 - a) * (1.0 + a)));
        return z < 0.0 ? low : 1.0 - low;
    }
    return 0.3 * (1.0 + z) + 0.2 + k * (skyAsinSmall(z) + z * sqrt((1.0 - z) * (1.0 + z)));
}

float sampleSkyVisibility(FrameBlock frame, vec3 point, vec3 normal) {
    if (any(equal(frame.skyDims.xyz, uvec3(0)))) return 0.0;
    ivec3 lim = ivec3(frame.skyDims.xyz) - 1;
    vec3 g = clamp((point - frame.skyOrigin.xyz) / frame.skyCellSize.xyz - 0.5, vec3(0.0), vec3(lim));
    ivec3 i0 = min(ivec3(floor(g)), max(lim - 1, ivec3(0)));
    vec3 f = g - vec3(i0);
    vec3 sq = normal * normal;
    float sum = 0.0, weight = 0.0;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                float w = (dx != 0 ? f.x : 1.0 - f.x) * (dy != 0 ? f.y : 1.0 - f.y) *
                          (dz != 0 ? f.z : 1.0 - f.z);
                if (w <= 0.0) continue;
                ivec3 p = min(i0 + ivec3(dx, dy, dz), lim);
                uint index = (uint(p.z) * frame.skyDims.y + uint(p.y)) * frame.skyDims.x + uint(p.x);
                SkyProbe probe = SkyProbeBuffer(frame.skyProbes).probes[index];
                if (probe.valid == 0u) continue;
                float response = sq.x * probe.value[normal.x < 0.0 ? 1 : 0] +
                                 sq.y * probe.value[normal.y < 0.0 ? 3 : 2] +
                                 sq.z * probe.value[normal.z < 0.0 ? 5 : 4];
                sum += w * response;
                weight += w;
            }
    return weight > 1e-9 ? skyAmbientResponse(normal.z) * sum / weight : 0.0;
}

vec3 irradiance(Light light, vec3 point, vec3 normal, FrameBlock frame) {
    if (light.kindMode.y == 0u) return vec3(0.0);
    uint kind = light.kindMode.x;
    if (kind == 3u) return light.intensityExponent.xyz * surfaceResponse(dot(normal, -light.directionInner.xyz));
    if (kind == 5u) return light.intensityExponent.xyz * sampleSkyVisibility(frame, point, normal);
    if (kind != 1u && kind != 2u) return vec3(0.0);
    vec3 delta = light.originRadius.xyz - point;
    float distance = length(delta);
    if (distance <= 0.0 || (light.originRadius.w > 0.0 && distance > light.originRadius.w)) return vec3(0.0);
    vec3 toLight = delta / distance;
    float cosine = dot(normal, toLight);
    if (cosine <= 0.0) return vec3(0.0);
    vec3 attn = light.attenuationOuter.xyz;
    float scale = surfaceResponse(cosine) / max(attn.x + attn.y * distance + attn.z * distance * distance, 1e-12);
    if (kind == 2u) {
        float along = dot(-toLight, light.directionInner.xyz);
        if (along <= light.attenuationOuter.w) return vec3(0.0);
        scale *= along;
        if (along < light.directionInner.w) {
            float edge = (along - light.attenuationOuter.w) / (light.directionInner.w - light.attenuationOuter.w);
            float exponent = light.intensityExponent.w;
            if (exponent != 0.0 && exponent != 1.0) edge = pow(edge, exponent);
            scale *= edge;
        }
    }
    return light.intensityExponent.xyz * scale;
}
#endif
