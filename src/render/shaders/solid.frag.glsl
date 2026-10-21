#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
// Unlit push-constant colour: edge lines and wireframe.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vMaterial;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = push.colour;
}
