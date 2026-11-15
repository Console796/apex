#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"

layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vMaterial;
layout(location = 5) in float vAlpha; // Unused; declared to match brush.vert's interface.

void main() {
    uint slot = pushSlot() == 0xFFFFFFFFu ? vMaterial : pushSlot();
    if (floatBitsToUint(push.p1.w) == 1u && sampleSlot(slot, vUv).a * push.colour.a < 0.5) discard;
}
