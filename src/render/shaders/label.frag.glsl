#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"

layout(location = 0) in vec4 vColour;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vColour.rgb, vColour.a * sampleSlot(pushSlot(), vUv).r);
}
