#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 scene = texelFetch(sampler2D(textures[nonuniformEXT(pushSlot())], samplers[1]), pixel, 0);
    uint depthSlot = floatBitsToUint(push.p0.x);
    gl_FragDepth = texelFetch(sampler2D(textures[nonuniformEXT(depthSlot)], samplers[1]), pixel, 0).r;
    // The game's display curve is the piecewise sRGB encode (fitted to pinned-exposure test1
    // captures, docs/plans/probe-results/captures_test1_0_6_release.md). SRGB attachments encode
    // in hardware; UNORM stores the encoded bytes.
    vec3 linear = clamp(scene.rgb * push.p0.y, 0.0, 1.0);
    vec3 display = floatBitsToUint(push.p0.z) != 0u ? linear :
        mix(1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, linear * 12.92, lessThanEqual(linear, vec3(0.0031308)));
    outColor = vec4(display, scene.a);
}
