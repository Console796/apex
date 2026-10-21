#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
// Camera-facing billboards, one instance per sprite, 6 vertices each. p0.xyz / p1.xyz are the
// camera's right and up vectors in world space.

layout(location = 0) in vec4 inPositionSize; // xyz relative to the draw origin, w half-size
layout(location = 1) in vec4 inColour;
layout(location = 2) in uint inSlot;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColour;
layout(location = 2) flat out uint vSlot;

const vec2 kCorners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    vec2 c = kCorners[gl_VertexIndex % 6];
    vec3 p = inPositionSize.xyz + push.offsetSlot.xyz + (push.p0.xyz * c.x + push.p1.xyz * c.y) * inPositionSize.w;
    gl_Position = push.viewProj * vec4(p, 1.0);
    vUv = vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    vColour = inColour;
    vSlot = inSlot;
}
