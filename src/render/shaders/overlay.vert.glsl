#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#include "common.inc"
// Editor overlay: vertex-less. The CPU expands lines and triangles to clip-space vertices; the
// address of the array arrives in p1.xy (uint bits).

struct OverlayVertex { vec4 position; vec4 colour; vec4 uv; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer OverlayBuffer {
    OverlayVertex vertices[];
};

layout(location = 0) out vec4 vColour;
layout(location = 1) out vec2 vUv;

void main() {
    OverlayVertex v = OverlayBuffer(floatBitsToUint(push.p1.xy)).vertices[gl_VertexIndex];
    gl_Position = v.position;
    vColour = v.colour;
    vColour.a *= push.colour.a;
    vUv = v.uv.xy;
}
