#ifndef APEX_MATERIAL_INC
#define APEX_MATERIAL_INC
#include "frame.inc"

const uint MATERIAL_HAS_BUMP = 1u;
const uint MATERIAL_SSBUMP = 2u;
const uint MATERIAL_LINEAR_BASE = 4u;
const uint MATERIAL_TWO_CHANNEL_NORMAL = 8u;
const uint MATERIAL_PBR = 16u;
const uint MATERIAL_PHONG = 32u;
const uint MATERIAL_BASE_ALPHA_PHONG_MASK = 64u;
const uint MATERIAL_ENVMAP = 128u;
const uint MATERIAL_ENVMAP_PROBE = 256u;
const uint MATERIAL_BASE_ALPHA_ENVMAP_MASK = 512u;
const uint MATERIAL_NORMAL_ALPHA_ENVMAP_MASK = 1024u;
const uint MATERIAL_SELFILLUM = 2048u;
const uint MATERIAL_LOCAL_REFRACT = 4096u;
const uint MATERIAL_PARALLAX = 8192u;
const uint MATERIAL_COLOUR2 = 16384u;
const uint NO_TEXTURE = 0xFFFFFFFFu;
// GpuMaterial in gpu_material.hpp; std430, 208-byte stride.
struct Material {
    uint baseSlot; uint bumpSlot; uint flags; uint selfIllumMaskSlot;
    uint mraoSlot; uint phongExponentSlot; uint envmapSlot; uint envmapMaskSlot;
    vec4 mraoScale;
    vec4 phongParams;
    vec4 phongFresnel;
    vec4 phongTint;
    vec4 envmapTint; // w = $envmaplightscale
    vec4 selfIllumTint;
    vec4 envmapParams; // x = $envmapcontrast, y = $envmapsaturation
    vec4 refractTint;
    vec4 refractParams; // x = $refractamount, y = $localrefract depth (world units), z = eta
    vec4 parallaxParams; // x = $parallaxdepth, y = $parallaxcenter, z = $parallaxscale
    vec4 colour2; // $color2, linear; applied with MATERIAL_COLOUR2
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MaterialBuffer {
    Material records[];
};
Material materialRecord(FrameBlock frame, uint slot) {
    if (slot < frame.materialCount) return MaterialBuffer(frame.materials).records[slot];
    return Material(0u, NO_TEXTURE, 0u, NO_TEXTURE, NO_TEXTURE, NO_TEXTURE, 0u, NO_TEXTURE,
                    vec4(1.0, 1.0, 1.0, 0.0), vec4(5.0, 1.0, 0.0, 0.0),
                    vec4(0.0, 0.5, 1.0, 0.0), vec4(1.0, 1.0, 1.0, 0.0),
                    vec4(1.0, 1.0, 1.0, 0.0), vec4(1.0, 1.0, 1.0, 0.0),
                    vec4(0.0, 1.0, 0.0, 0.0), vec4(1.0, 1.0, 1.0, 0.0), vec4(0.0),
                    vec4(0.0, 0.0, 1.0, 0.0), vec4(1.0, 1.0, 1.0, 0.0));
}

// GpuCubemapProbe in gpu_lighting.hpp; std430, 80-byte stride. Camera-relative positions.
struct CubemapProbe {
    vec3 position;
    uint cube;
    mat4 worldToBox;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer CubemapProbeBuffer {
    CubemapProbe probes[];
};
// The env_cubemap nearest to the point (first on ties), or the default cube without probes.
uint nearestCubemapIndex(FrameBlock frame, vec3 point) {
    uint cube = 0u;
    float best = 3.4e38;
    for (uint i = 0u; i < frame.cubemapCount; ++i) {
        CubemapProbe probe = CubemapProbeBuffer(frame.cubemaps).probes[i];
        vec3 delta = probe.position - point;
        float distance = dot(delta, delta);
        if (distance < best) {
            best = distance;
            cube = i;
        }
    }
    return cube;
}
uint nearestCubemap(FrameBlock frame, vec3 point) {
    return frame.cubemapCount == 0u ? 0u :
        CubemapProbeBuffer(frame.cubemaps).probes[nearestCubemapIndex(frame, point)].cube;
}
vec3 correctedReflection(CubemapProbe probe, vec3 point, vec3 ray) {
    if (probe.worldToBox[3][3] == 0.0) return ray;
    vec3 local = (probe.worldToBox * vec4(point, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(1.0)))) return ray;
    vec3 direction = mat3(probe.worldToBox) * ray;
    vec3 safeRay = mix(vec3(1e-20), direction, greaterThan(abs(direction), vec3(1e-20)));
    vec3 farHit = max((-vec3(1.0) - local) / safeRay, (vec3(1.0) - local) / safeRay);
    float distance = min(farHit.x, min(farHit.y, farHit.z));
    return point + ray * distance - probe.position;
}
#endif
