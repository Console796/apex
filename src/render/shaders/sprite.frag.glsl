#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColour;
layout(location = 2) flat in uint vSlot;
layout(location = 0) out vec4 outColor;

void main() {
    if ((floatBitsToUint(push.p0.w) & 1u) != 0) {
        outColor = push.colour;
        return;
    }
    vec4 texel = sampleSlot(vSlot, vUv) * vColour;
    texel.rgb = mix(texel.rgb, push.colour.rgb, push.colour.a);
    if (texel.a < 0.5) discard;
    outColor = vec4(texel.rgb, 1.0);
}
