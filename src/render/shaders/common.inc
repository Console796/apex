// Shared by every shader (plan 0.1.9). Push constants are 128 bytes, identical for all pipelines
// (mirrors PushConstants in render/renderer.hpp).
layout(push_constant) uniform Push {
    mat4 viewProj;   // Rotation-only view * projection; grid also folds in its plane transform.
    vec4 colour;
    vec4 offsetSlot; // xyz: draw origin relative to the camera; w: texture slot (uint bits). Grid: fade origin xy, distance, major phase v.
    vec4 p0;         // Brush: highlight; grid: minor phase uv, major phase u, spacing; sprite: packed index/outline in w; box: x = solid, y = outline.
    vec4 p1;         // Grid: plane rect; mesh/box/sprite: frame address low/high; mesh/box: index, mesh alpha/flags (uint bits).
} push;

// Tone map reuses offsetSlot.w for scene colour, p0.x for depth (uint bits), p0.y for exposure, p0.z bit 0 for SRGB attachment encoding.
uint pushSlot() { return floatBitsToUint(push.offsetSlot.w); }

// Gamma-encoded texture colour to linear: the piecewise sRGB decode, as hardware SRGB reads do. The
// tone map encodes with the inverse, so Lit fullbright reproduces a texture exactly.
vec3 srgbToLinear(vec3 c) {
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
}
