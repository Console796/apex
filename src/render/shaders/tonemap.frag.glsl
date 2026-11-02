#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"

layout(location = 0) out vec4 outColor;

// Horizon-based AO from the scene depth (plans/0.7-realtime-gi.md, option A). Reversed-Z with an
// infinite far plane: view z = near / depth. The projection's scale and near plane come out of the
// rotation-only viewProj: its rows 0 and 1 have length P00 and P11, and clip.z is the constant near.
// push.colour: x strength, y radius (map units).
#define DEPTH_TEX(slot) sampler2D(textures[nonuniformEXT(slot)], samplers[1])
float depthAt(uint slot, ivec2 pixel) {
    return texelFetch(DEPTH_TEX(slot), clamp(pixel, ivec2(0), textureSize(DEPTH_TEX(slot), 0) - 1), 0).r;
}

vec3 viewPosition(float depth, vec2 pixel, vec2 size, vec2 invScale, float near) {
    float z = near / depth;
    vec2 ndc = pixel / size * 2.0 - 1.0;
    return vec3(ndc * invScale * z, z);
}

float ambientVisibility(uint depthSlot, ivec2 pixel, float strength, float radius) {
    mat4 m = push.viewProj;
    bool perspective = abs(m[3][3]) < 1e-6 && m[3][2] > 0.0;
    float centre = depthAt(depthSlot, pixel);
    if (strength <= 0.0 || !perspective || centre <= 0.0) return 1.0;
    vec2 size = vec2(textureSize(DEPTH_TEX(depthSlot), 0));
    vec2 invScale = 1.0 / vec2(length(vec3(m[0][0], m[1][0], m[2][0])), length(vec3(m[0][1], m[1][1], m[2][1])));
    float near = m[3][2];
    vec3 p = viewPosition(centre, vec2(pixel) + 0.5, size, invScale, near);
    // Normal from the nearer neighbour on each axis, so edges do not smear.
    vec3 px1 = viewPosition(depthAt(depthSlot, pixel + ivec2(1, 0)), vec2(pixel) + vec2(1.5, 0.5), size, invScale, near);
    vec3 px0 = viewPosition(depthAt(depthSlot, pixel - ivec2(1, 0)), vec2(pixel) + vec2(-0.5, 0.5), size, invScale, near);
    vec3 py1 = viewPosition(depthAt(depthSlot, pixel + ivec2(0, 1)), vec2(pixel) + vec2(0.5, 1.5), size, invScale, near);
    vec3 py0 = viewPosition(depthAt(depthSlot, pixel - ivec2(0, 1)), vec2(pixel) + vec2(0.5, -0.5), size, invScale, near);
    vec3 dx = abs(px1.z - p.z) < abs(p.z - px0.z) ? px1 - p : p - px0;
    vec3 dy = abs(py1.z - p.z) < abs(p.z - py0.z) ? py1 - p : p - py0;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n; // Face the camera.

    // Screen-space radius of the world radius at this depth, capped so near walls stay cheap.
    float radiusPixels = min(radius / p.z * size.y * 0.5 / invScale.y, size.y * 0.15);
    if (radiusPixels < 2.0) return 1.0;
    float noise = fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715))));
    const int directions = 6, steps = 6;
    float occlusion = 0.0;
    for (int d = 0; d < directions; ++d) {
        float angle = (float(d) + noise) * (3.14159265 / float(directions));
        vec2 dir = vec2(cos(angle), sin(angle));
        for (int side = 0; side < 2; ++side) {
            vec2 stepDir = side == 0 ? dir : -dir;
            float best = 0.0;
            for (int s = 1; s <= steps; ++s) {
                float t = (float(s) - 1.0 + fract(noise * 7.0)) / float(steps);
                vec2 samplePixel = vec2(pixel) + 0.5 + stepDir * (1.0 + t * t * radiusPixels);
                float sampleDepth = depthAt(depthSlot, ivec2(samplePixel));
                if (sampleDepth <= 0.0) continue; // Sky or cleared: no occluder.
                vec3 v = viewPosition(sampleDepth, samplePixel, size, invScale, near) - p;
                float len = length(v);
                if (len < 1e-4) continue;
                float falloff = clamp(1.0 - (len * len) / (radius * radius), 0.0, 1.0);
                best = max(best, (dot(n, v) / len - 0.15) * falloff);
            }
            occlusion += max(best, 0.0);
        }
    }
    return clamp(1.0 - occlusion / float(directions * 2) * 1.6, 0.0, 1.0);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 scene = texelFetch(sampler2D(textures[nonuniformEXT(pushSlot())], samplers[1]), pixel, 0);
    uint depthSlot = floatBitsToUint(push.p0.x);
    gl_FragDepth = texelFetch(DEPTH_TEX(depthSlot), pixel, 0).r;
    // AO only has a fill term to darken, so it is weighted to the dim pixels; bright, directly lit
    // ones keep the measured direct light.
    float visibility = ambientVisibility(depthSlot, pixel, push.colour.x, push.colour.y);
    float luma = dot(scene.rgb * push.p0.y, vec3(0.2126, 0.7152, 0.0722));
    scene.rgb *= mix(1.0, visibility, push.colour.x * (1.0 - smoothstep(0.02, 0.3, luma)));
    // The game's display curve is the piecewise sRGB encode (fitted to pinned-exposure test1
    // captures, docs/plans/probe-results/captures_test1_0_6_release.md). SRGB attachments encode
    // in hardware; UNORM stores the encoded bytes.
    vec3 linear = clamp(scene.rgb * push.p0.y, 0.0, 1.0);
    vec3 display = floatBitsToUint(push.p0.z) != 0u ? linear :
        mix(1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, linear * 12.92, lessThanEqual(linear, vec3(0.0031308)));
    outColor = vec4(display, scene.a);
}
