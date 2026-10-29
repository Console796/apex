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
    // Placeholder until the game's curve is fitted in 0.6.4. Preserve display-space alpha.
    outColor = vec4(clamp(scene.rgb * push.p0.y, 0.0, 1.0), scene.a);
}
