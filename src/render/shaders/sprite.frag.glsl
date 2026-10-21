#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColour;
layout(location = 2) flat in uint vSlot;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = sampleSlot(vSlot, vUv) * vColour;
    if (texel.a < 0.5) discard;
    outColor = vec4(texel.rgb, 1.0);
}
