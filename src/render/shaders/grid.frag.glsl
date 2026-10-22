#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
// p0.xy: projected camera modulo spacing; p0.z / offsetSlot.w: major phase; p0.w: spacing.
// offsetSlot.xyz: fade origin in camera-relative plane coordinates, fade distance.
// colour.rgb: line colour, colour.a: peak opacity. Every 8th line is brighter.

layout(location = 0) in vec2 vPlane;
layout(location = 0) out vec4 outColor;

float lineMask(vec2 phase, float spacing, vec2 planeWidth) {
    // Reduce the camera-relative coordinate before adding the CPU-wrapped phase. In
    // particular, never differentiate a microgrid coordinate with a large phase added.
    vec2 coord = fract(mod(vPlane, spacing) / spacing + phase / spacing);
    vec2 width = max(planeWidth / spacing, vec2(1e-6));
    vec2 g = min(coord, 1.0 - coord) / width;
    // Fade each line direction independently between 4 and 2 pixels of pitch. Each set
    // uses its own spacing, so majors remain readable after the minor lines disappear.
    vec2 visibility = 1.0 - smoothstep(vec2(0.25), vec2(0.5), width);
    vec2 lines = (1.0 - min(g, vec2(1.0))) * visibility;
    return max(lines.x, lines.y);
}

void main() {
    // Perspective-correct plane coordinates are N / W, where N and W (= reciprocal
    // clip w) are affine in screen space. Differentiate those affine quantities, then
    // apply the quotient rule at this pixel. Quad derivatives of vPlane itself sample
    // different points on the rational function and change width/fade across a quad.
    vec2 numerator = vPlane * gl_FragCoord.w;
    vec2 dx = (dFdx(numerator) - vPlane * dFdx(gl_FragCoord.w)) / gl_FragCoord.w;
    vec2 dy = (dFdy(numerator) - vPlane * dFdy(gl_FragCoord.w)) / gl_FragCoord.w;
    vec2 planeWidth = abs(dx) + abs(dy);
    float spacing = push.p0.w;
    float minor = lineMask(push.p0.xy, spacing, planeWidth);
    float major = lineMask(vec2(push.p0.z, push.offsetSlot.w), spacing * 8.0, planeWidth);
    float a = max(minor * 0.45, major);
    vec3 rgb = push.colour.rgb;
    float distance = length(vPlane - push.offsetSlot.xy);
    float fade = 1.0 - smoothstep(push.offsetSlot.z * 0.5, push.offsetSlot.z, distance);
    outColor = vec4(rgb, a * fade * push.colour.a);
    if (outColor.a <= 0.002) discard;
}
