#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
// p0.xyz: camera origin modulo 1024 (exact; spacing divides 1024), p0.w: minor spacing.
// colour.rgb: line colour, colour.a: peak opacity. Every 8th line is brighter.

layout(location = 0) in vec2 vPlane;
layout(location = 0) out vec4 outColor;

float lineMask(vec2 coord) {
    vec2 g = abs(fract(coord - 0.5) - 0.5) / max(fwidth(coord), vec2(1e-6));
    return 1.0 - min(min(g.x, g.y), 1.0);
}

void main() {
    float spacing = push.p0.w;
    vec2 world = vPlane + push.p0.xy; // Relative to a 1024-aligned origin.
    float minor = lineMask(world / spacing);
    float major = lineMask(world / (spacing * 8.0));
    float a = max(minor * 0.45, major);
    vec3 rgb = push.colour.rgb;
    float fade = 1.0 - smoothstep(push.p1.x * 0.5, push.p1.x, length(vPlane));
    outColor = vec4(rgb, a * fade * push.colour.a);
    if (outColor.a <= 0.002) discard;
}
