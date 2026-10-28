#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColour;
layout(location = 2) flat in uint vSlot;
layout(location = 0) out vec4 outColor;

void main() {
    if ((floatBitsToUint(push.p0.w) & 1u) != 0) {
        outColor = push.colour;
        return;
    }
    // Bit 31 of the slot asks for the nearest sampler (the editor's icon filtering preference).
    const uint sampler_ = (vSlot >> 31) != 0u ? 1u : 0u;
    vec4 texel = texture(sampler2D(textures[nonuniformEXT(vSlot & 0x7FFFFFFFu)], samplers[sampler_]), vUv) * vColour;
    texel.rgb = mix(texel.rgb, push.colour.rgb, push.colour.a);
    if (texel.a < 0.5) discard;
    outColor = vec4(texel.rgb, 1.0);
}
