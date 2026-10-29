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
    // Placeholder display curve until the game's curve is fitted. UNORM stores these bytes directly.
    vec3 display = pow(clamp(scene.rgb * push.p0.y, 0.0, 1.0), vec3(1.0 / 2.2));
    if (floatBitsToUint(push.p0.z) != 0u) {
        // SRGB attachments encode in hardware. Feed linear SRGB, preserving the same placeholder
        // curve as UNORM: the hardware's piecewise SRGB curve differs from a simple 2.2 power.
        display = mix(pow((display + 0.055) / 1.055, vec3(2.4)), display / 12.92,
                      lessThanEqual(display, vec3(0.04045)));
    }
    outColor = vec4(display, scene.a);
}
