#ifndef APEX_SHADOWS_INC
#define APEX_SHADOWS_INC

// GpuShadowTile in shadows.hpp; std430, 96-byte stride. params: y normal offset, z offset per unit distance.
struct ShadowTile { mat4 projection; vec4 rect; vec4 params; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowBuffer { ShadowTile tiles[]; };

const float SHADOW_TEXEL = 1.0 / 4096.0;

// 3x3 PCF through the comparison sampler (reversed-Z: GREATER_OR_EQUAL). Taps clamp to the tile, so a tile
// never reads its neighbour; point faces carry a guard texel so the clamp is never visible at a seam (D13).
float shadowPcf(FrameBlock frame, ShadowTile tile, vec3 ndc) {
    vec2 uv = tile.rect.xy + (ndc.xy * 0.5 + 0.5) * tile.rect.zw;
    vec2 lo = tile.rect.xy + vec2(SHADOW_TEXEL * 0.5);
    vec2 hi = tile.rect.xy + tile.rect.zw - vec2(SHADOW_TEXEL * 0.5);
    float visible = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            visible += textureLod(sampler2DShadow(textures[nonuniformEXT(frame.shadowAtlas)], samplers[2]),
                                  vec3(clamp(uv + vec2(x, y) * SHADOW_TEXEL, lo, hi), ndc.z), 0.0);
    return visible / 9.0;
}

float shadowVisibility(FrameBlock frame, Light light, vec3 point, vec3 normal) {
    uint first = light.shadow.x;
    if (first == 0u || first > frame.shadowCount) return 1.0;
    first -= 1u;
    ShadowBuffer buf = ShadowBuffer(frame.shadows);
    uint kind = light.kindMode.x;
    if (kind == 3u) {
        // Sun: the first (finest) cascade whose box contains the point.
        for (uint i = 0u; i < light.shadow.y; ++i) {
            ShadowTile tile = buf.tiles[first + i];
            vec4 clip = tile.projection * vec4(point + normal * tile.params.y, 1.0);
            vec3 ndc = clip.xyz / clip.w;
            float margin = 4.0 / (tile.rect.z * 4096.0);
            if (any(greaterThan(abs(ndc.xy), vec2(1.0 - margin))) || ndc.z < 0.0 || ndc.z > 1.0) continue;
            return shadowPcf(frame, tile, ndc);
        }
        return 1.0;
    }
    float dist = length(point - light.originRadius.xyz);
    ShadowTile tile = buf.tiles[first];
    vec3 offset = normal * (tile.params.y + tile.params.z * dist);
    if (light.shadow.y == 6u) {
        // Cube: face by the major axis of the offset position. +X -X +Y -Y +Z -Z.
        vec3 d = point + offset - light.originRadius.xyz;
        vec3 a = abs(d);
        uint face = a.x >= a.y && a.x >= a.z ? (d.x >= 0.0 ? 0u : 1u) : a.y >= a.z ? (d.y >= 0.0 ? 2u : 3u) : (d.z >= 0.0 ? 4u : 5u);
        tile = buf.tiles[first + face];
    }
    vec4 clip = tile.projection * vec4(point + offset, 1.0);
    if (clip.w <= 0.0) return 1.0;
    vec3 ndc = clip.xyz / clip.w;
    if (any(greaterThan(abs(ndc.xy), vec2(1.0))) || ndc.z < 0.0 || ndc.z > 1.0) return 1.0;
    return shadowPcf(frame, tile, ndc);
}
#endif
