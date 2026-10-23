#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vMaterial;
layout(location = 0) out vec4 outColor;

void main() {
    // The draw's texture slot, or the per-vertex material index when the draw passes 0xFFFFFFFF.
    uint slot = pushSlot() == 0xFFFFFFFFu ? vMaterial : pushSlot();
    float shade = 0.65 + 0.35 * abs(dot(normalize(vNormal), normalize(vec3(0.3, 0.5, 0.8))));
    vec4 texel = sampleSlot(slot, vUv);
    outColor = vec4(texel.rgb * push.colour.rgb * shade, push.colour.a);
    outColor.rgb = mix(outColor.rgb, push.p0.rgb, push.p0.a);
}
