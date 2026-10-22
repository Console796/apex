#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
// viewProj includes the plane basis and camera-relative normal offset.
// p1: camera-relative plane rectangle (uMin, uMax, vMin, vMax).
// The sheet is a kCells x kCells mesh, not two huge triangles: hardware interpolates varyings
// in fp32, and across an 8192-unit triangle the error is a visible fraction of a 0.125 cell.

layout(location = 0) out vec2 vPlane; // relative to the camera's projection onto the plane

const int kCells = 64;
const vec2 kCorners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));

void main() {
    int cell = gl_VertexIndex / 6;
    vec2 corner = vec2(cell % kCells, cell / kCells) + kCorners[gl_VertexIndex % 6];
    vec2 t = corner / float(kCells);
    vPlane = vec2(mix(push.p1.x, push.p1.y, t.x), mix(push.p1.z, push.p1.w, t.y));
    gl_Position = push.viewProj * vec4(vPlane, 0.0, 1.0);
}
