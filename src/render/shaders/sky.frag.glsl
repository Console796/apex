#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"
#include "frame.inc"
layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;
void main() {
    FrameBlock frame = frameBlock();
    vec3 ray = (frame.skyInverseView * vec4(vNdc, 0.0, 1.0)).xyz;
    vec3 a = abs(ray);
    uint face;
    vec2 uv;
    // Cube convention; suffix orientation remains provisional (D2).
    if (a.x >= a.y && a.x >= a.z) {
        face = ray.x > 0.0 ? 0u : 1u;
        uv = vec2(ray.x > 0.0 ? -ray.z : ray.z, -ray.y) / a.x;
    } else if (a.y >= a.z) {
        face = ray.y > 0.0 ? 2u : 3u;
        uv = vec2(ray.x, ray.y > 0.0 ? ray.z : -ray.z) / a.y;
    } else {
        face = ray.z > 0.0 ? 4u : 5u;
        uv = vec2(ray.z > 0.0 ? ray.x : -ray.x, -ray.y) / a.z;
    }
    uint slot = frame.skyFaces[face / 4u][face % 4u];
    ivec2 size = textureSize(sampler2D(textures[nonuniformEXT(slot)], samplers[0]), 0);
    uv = clamp(uv * 0.5 + 0.5, 0.5 / vec2(size), 1.0 - 0.5 / vec2(size));
    vec3 colour = textureLod(sampler2D(textures[nonuniformEXT(slot)], samplers[0]), uv, 0.0).rgb;
    if (push.p0.x > 0.0) {
        if ((frame.skyFaces[1].z & (1u << face)) == 0u) colour = pow(colour, vec3(2.2));
        if (frame.fogParams.w > 0.0) colour = mix(colour, frame.fogColour.rgb, frame.fogParams.z);
    }
    outColor = vec4(colour, 1.0);
}
