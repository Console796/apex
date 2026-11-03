#ifndef APEX_PROBES_INC
#define APEX_PROBES_INC
#include "frame.inc"

const uint PROBE_SIDE = 8u;
const uint PROBE_TEXELS = 64u;
// Relocation offsets ride in the depth texels' free channels: x/y in depth[0].zw, z in depth[1].z,
// stored as a fraction of the grid spacing so they survive grid rebasing unchanged.
struct IrradianceProbe {
    vec4 irradiance[64];
    vec4 depth[64];
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ProbeBuffer {
    IrradianceProbe probes[];
};

vec2 octSign(vec2 v) { return mix(vec2(-1.0), vec2(1.0), greaterThanEqual(v, vec2(0.0))); }
vec2 octEncode(vec3 v) {
    v /= abs(v.x) + abs(v.y) + abs(v.z);
    return (v.z >= 0.0 ? v.xy : (1.0 - abs(v.yx)) * octSign(v.xy)) * 0.5 + 0.5;
}
vec3 octDecode(vec2 uv) {
    vec3 n = vec3(uv * 2.0 - 1.0, 0.0);
    n.z = 1.0 - abs(n.x) - abs(n.y);
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * octSign(n.xy);
    return normalize(n);
}
uint probeTexel(ivec2 p) {
    // Octahedral edge continuation, including corners, without duplicated border texels.
    if (p.x < 0) { p.x = -p.x - 1; p.y = 7 - p.y; }
    if (p.x > 7) { p.x = 15 - p.x; p.y = 7 - p.y; }
    if (p.y < 0) { p.y = -p.y - 1; p.x = 7 - p.x; }
    if (p.y > 7) { p.y = 15 - p.y; p.x = 7 - p.x; }
    return uint(p.y * 8 + p.x);
}
vec4 probeMap(ProbeBuffer maps, uint index, vec3 direction, bool depth) {
    vec2 uv = octEncode(direction) * 8.0 - 0.5;
    ivec2 p = ivec2(floor(uv));
    vec2 f = fract(uv);
    vec4 result = vec4(0.0);
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        uint texel = probeTexel(p + ivec2(x, y));
        float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
        result += w * (depth ? maps.probes[index].depth[texel] : maps.probes[index].irradiance[texel]);
    }
    return result;
}
// Weight of a point inside a cascade's own coverage, fading to its edge over `fadeCells` cells.
// Outside the coverage (g outside [0, dims-1]) returns 0.
float cascadeEdgeFade(vec3 g, uvec3 dims, float fadeCells) {
    if (any(lessThan(g, vec3(0.0))) || any(greaterThan(g, vec3(dims - 1u)))) return 0.0;
    vec3 inset = min(g, vec3(dims - 1u) - g);
    return clamp(min(min(inset.x, inset.y), inset.z) / max(fadeCells, 1e-3), 0.0, 1.0);
}

vec3 probeIrradiance(uvec2 address, uvec3 dims, vec3 origin, float spacing, float bias,
                     vec3 point, vec3 normal) {
    if (any(equal(dims, uvec3(0))) || address.x == 0u) return vec3(0.0);
    vec3 biased = point + normal * bias;
    vec3 g = (biased - origin) / spacing;
    if (any(lessThan(g, vec3(0.0))) || any(greaterThan(g, vec3(dims - 1u)))) return vec3(0.0);
    ivec3 base = min(ivec3(floor(g)), ivec3(dims) - 2);
    vec3 f = g - vec3(base);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    ProbeBuffer maps = ProbeBuffer(address);
    for (int z = 0; z < 2; ++z) for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        ivec3 cell = base + ivec3(x, y, z);
        uint index = (uint(cell.z) * dims.y + uint(cell.y)) * dims.x + uint(cell.x);
        if (maps.probes[index].irradiance[0].w <= 0.0) continue;
        // Relocated probes sample from where they actually sit, not their grid slot.
        vec3 offset = vec3(maps.probes[index].depth[0].zw, maps.probes[index].depth[1].z) * spacing;
        vec3 delta = biased - (origin + vec3(cell) * spacing + offset);
        float distance = length(delta);
        vec3 direction = distance > 1e-5 ? delta / distance : normal;
        // Surface-side weighting: a probe behind the shading plane (a thin wall's far side)
        // cannot light this point, so its interpolation weight collapses to zero instead of
        // relying only on the Chebyshev distance test below. `direction` runs from the probe
        // toward the point, so the normal's dot flips sign against it.
        float side = max(0.5 + 0.5 * dot(normal, -direction), 0.0);
        vec2 moments = probeMap(maps, index, direction, true).xy;
        float variance = max(moments.y - moments.x * moments.x, 0.0);
        float excess = max(distance - moments.x, 0.0);
        float visibility = excess == 0.0 ? 1.0 : variance / max(variance + excess * excess, 1e-6);
        visibility = visibility * visibility * visibility;
        float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y) *
                  (z == 0 ? 1.0 - f.z : f.z) * side * side;
        // Keep the geometric interpolation denominator: occlusion must not normalise a tiny
        // wrong-room contribution back to full brightness when all visible probes are dark.
        total += w;
        sum += w * visibility * probeMap(maps, index, normal, false).rgb;
    }
    return total > 1e-6 ? sum / total : vec3(0.0);
}

// Nested cascades: the coarse cascade covers the fine grid's edge band (and everything beyond),
// so the fine result cross-fades out over `fadeCells` rather than stepping to black at coverage.
vec3 cascadeIrradiance(uvec2 fine, uvec3 fineDims, vec3 fineOrigin, float fineSpacing, float fineBias, float fineFade,
                       uvec2 coarse, uvec3 coarseDims, vec3 coarseOrigin, float coarseSpacing, float coarseBias,
                       vec3 point, vec3 normal) {
    float weight = 0.0;
    vec3 fineResult = vec3(0.0);
    if (fineDims.x != 0u && fine.x != 0u) {
        vec3 biased = point + normal * fineBias;
        weight = cascadeEdgeFade((biased - fineOrigin) / fineSpacing, fineDims, fineFade);
        if (weight > 0.0) fineResult = probeIrradiance(fine, fineDims, fineOrigin, fineSpacing, fineBias, point, normal);
        if (weight >= 1.0) return fineResult;
    }
    vec3 result = vec3(0.0);
    if (coarseDims.x != 0u && coarse.x != 0u) {
        // The outer cascade fades against its own one coarse cell, softening its coverage edge.
        vec3 coarseG = (point + normal * coarseBias - coarseOrigin) / coarseSpacing;
        result = probeIrradiance(coarse, coarseDims, coarseOrigin, coarseSpacing, coarseBias, point, normal) *
                 cascadeEdgeFade(coarseG, coarseDims, 1.0);
    }
    return mix(result, fineResult, weight);
}
vec3 sampleIndirect(FrameBlock frame, vec3 point, vec3 normal) {
    return cascadeIrradiance(frame.indirectProbes, frame.indirectDims.xyz, frame.indirectOrigin.xyz,
                             frame.indirectParams.x, frame.indirectParams.y, frame.indirectParams.z,
                             frame.indirectCoarse, frame.indirectCoarseDims.xyz, frame.indirectCoarseOrigin.xyz,
                             frame.indirectCoarseParams.x, frame.indirectCoarseParams.y, point, normal);
}
#endif
