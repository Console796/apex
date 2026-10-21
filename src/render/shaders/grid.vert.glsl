#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
// Ground-plane grid: one huge quad on z = p1.y (camera-relative) centred under the camera.
// p1.x is the fade distance; the quad extends 2x that far.

layout(location = 0) out vec2 vPlane; // camera-relative xy

const vec2 kCorners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    vec2 xy = kCorners[gl_VertexIndex % 6] * push.p1.x * 2.0;
    vPlane = xy;
    gl_Position = push.viewProj * vec4(xy, push.p1.y, 1.0);
}
