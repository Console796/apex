#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
// Brush faces and edges: camera-relative position, normal, uv, material index.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in uint inMaterial;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out uint vMaterial;

void main() {
    gl_Position = push.viewProj * vec4(inPosition + push.offsetSlot.xyz, 1.0);
    vNormal = inNormal;
    vUv = inUv;
    vMaterial = inMaterial;
}
