#version 450
#extension GL_GOOGLE_include_directive : require
#define APEX_FRAME_NO_PUSH
#include "../../../src/render/shaders/lighting.inc"
#include "../../../src/render/shaders/normal_map.inc"
#include "../../../src/render/shaders/specular.inc"
#include "../../../src/render/shaders/material.inc"

layout(local_size_x = 1) in;
struct Sample { Light light; vec4 point; vec4 normal; uvec2 frame; uvec2 pad; vec4 texel; vec4 dpdx; vec4 dpdy; vec4 uv; Material material; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Samples { Sample samples[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer Output { vec4 values[]; };
layout(push_constant) uniform Probe { uvec2 inputAddress; uvec2 outputAddress; } probe;

void main() {
    uint i = gl_GlobalInvocationID.x;
    Sample s = Samples(probe.inputAddress).samples[i];
    if (s.pad.y >= 5u) {
        // Envmap and self-illumination terms; the material fields check the record ABI too.
        Material m = s.material;
        vec3 value = s.pad.y == 5u ? envBrdf(s.texel.rgb, s.uv.x, s.uv.y) * envmapLightScale(s.dpdx.xyz, m.envmapTint.w) :
            s.pad.y == 9u ? vec3(localRefractUv(s.uv.xy, s.texel.xyz, s.dpdx.xyz, m.refractParams.x, m.refractParams.y), 0.0) :
            s.pad.y == 8u ? envmapContrastSaturation(s.texel.rgb, m.envmapParams.x, m.envmapParams.y) :
            s.pad.y == 6u ? m.envmapTint.rgb * envmapMask(m.envmapMaskSlot != NO_TEXTURE, s.dpdx.xyz,
                (m.flags & MATERIAL_NORMAL_ALPHA_ENVMAP_MASK) != 0u, s.uv.x,
                (m.flags & MATERIAL_BASE_ALPHA_ENVMAP_MASK) != 0u, s.texel.w) :
            m.selfIllumTint.rgb * selfIllumMask(m.selfIllumMaskSlot != NO_TEXTURE, s.uv.y, s.texel.w);
        Output(probe.outputAddress).values[i] = vec4(value, 0.0);
        return;
    }
    if (s.pad.y != 0u) {
        Material m = s.material;
        float exponent = m.phongExponentSlot == NO_TEXTURE ? m.phongParams.x : phongTextureExponent(s.uv.x);
        vec3 brdf = s.pad.y % 2u == 1u ?
            ggxBrdf(s.normal.xyz, s.dpdx.xyz, s.dpdy.xyz, s.texel.rgb, m.mraoScale.x, m.mraoScale.y) :
            phongBrdf(s.normal.xyz, s.dpdx.xyz, s.dpdy.xyz, exponent, m.phongParams.y,
                      m.phongFresnel.xyz, m.phongTint.xyz, s.texel.w);
        if (s.pad.y > 2u) brdf = specularIrradiance(
            irradiance(s.light, s.point.xyz, s.normal.xyz, FrameBlock(s.frame)), dot(s.normal.xyz, s.dpdy.xyz), brdf);
        Output(probe.outputAddress).values[i] = vec4(brdf, 0.0);
        return;
    }
    if (s.texel.w != 0.0) {
        vec3 normal = decodeNormal(s.texel.xyz, s.texel.w == 2.0 || s.texel.w == 4.0, s.pad.x != 0u);
        if (s.texel.w >= 3.0) normal = perturbNormal(s.normal.xyz, s.dpdx.xyz, s.dpdy.xyz, s.uv.xy, s.uv.zw, normal);
        Output(probe.outputAddress).values[i] = vec4(normal, 0.0);
        return;
    }
    Output(probe.outputAddress).values[i] = vec4(irradiance(s.light, s.point.xyz, s.normal.xyz, FrameBlock(s.frame)), 0.0);
}
