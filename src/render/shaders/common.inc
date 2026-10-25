// Shared by every shader (plan 0.1.9). Push constants are 128 bytes, identical for all pipelines
// (mirrors PushConstants in render/renderer.hpp).
layout(push_constant) uniform Push {
    mat4 viewProj;   // Rotation-only view * projection; grid also folds in its plane transform.
    vec4 colour;
    vec4 offsetSlot; // xyz: draw origin relative to the camera; w: texture slot (uint bits). Grid: fade origin xy, distance, major phase v.
    vec4 p0;         // Brush: highlight; grid: minor phase uv, major phase u, spacing; sprite: right, packed index/outline; box: x = solid, y = outline.
    vec4 p1;         // Grid: plane rect; sprite: up, address high; brush/box: address low/high, index, brush alpha mode (uint bits).
} push;

uint pushSlot() { return floatBitsToUint(push.offsetSlot.w); }
