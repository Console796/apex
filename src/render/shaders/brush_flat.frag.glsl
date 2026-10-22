#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vMaterial;
layout(location = 0) out vec4 outColor;

void main() {
    float shade = 0.55 + 0.45 * abs(dot(normalize(vNormal), normalize(vec3(0.3, 0.5, 0.8))));
    outColor = vec4(push.colour.rgb * shade, push.colour.a);
    outColor.rgb = mix(outColor.rgb, push.p0.rgb, push.p0.a);
}
