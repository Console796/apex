#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"
#include "frame.inc"
#include "lighting.inc"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vMaterial;
layout(location = 3) in vec3 vPosition;
layout(location = 0) out vec4 outColor;

void main() {
    // The draw's texture slot, or the per-vertex material index when the draw passes 0xFFFFFFFF.
    uint slot = pushSlot() == 0xFFFFFFFFu ? vMaterial : pushSlot();
    float shade = 0.65 + 0.35 * abs(dot(normalize(vNormal), normalize(vec3(0.3, 0.5, 0.8))));
    vec4 texel = sampleSlot(slot, vUv);
    // p1.w (unused by the brush vertex shader): 0 opaque, 1 alpha test, 2 blend (AlphaMode);
    // bit 2 outputs only the highlight tint, covering what the mesh covers.
    uint alphaMode = floatBitsToUint(push.p1.w) & 3u;
    bool tintOnly = (floatBitsToUint(push.p1.w) & 4u) != 0u;
    float alpha = alphaMode == 0u ? push.colour.a : push.colour.a * texel.a;
    if (alphaMode == 1u) {
        if (alpha < 0.5) discard;
        alpha = 1.0;
    }
    outColor = vec4(texel.rgb * push.colour.rgb * shade, alpha);
    if ((floatBitsToUint(push.p1.w) & 8u) != 0u) {
        FrameBlock frame = frameBlock();
        vec3 sum = vec3(0.0);
        if ((frame.flags & 1u) != 0u) sum = vec3(1.0);
        else {
            // Styles (including named/nonzero styles) are held at full brightness for now.
            for (uint i = 0u; i < frame.lightCount; ++i)
                sum += irradiance(LightBuffer(frame.lights).lights[i], vPosition, normalize(vNormal));
        }
        // Heap material formats are UNORM (also BCn), so sampling does not decode colour.
        vec3 albedo = pow(max(texel.rgb * push.colour.rgb, vec3(0.0)), vec3(2.2));
        outColor = vec4(albedo * sum, alpha);
    }
    outColor.rgb = mix(outColor.rgb, push.p0.rgb, push.p0.a);
    if (tintOnly) outColor = vec4(push.p0.rgb, push.p0.a * alpha);
}
