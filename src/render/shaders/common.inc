// Shared by every shader (plan 0.1.9). Push constants are 128 bytes, identical for all pipelines
// (mirrors PushConstants in render/renderer.hpp).
layout(push_constant) uniform Push {
    mat4 viewProj;   // Rotation-only view * projection; grid also folds in its plane transform.
    vec4 colour;
    vec4 offsetSlot; // xyz: draw origin relative to the camera; w: texture slot (uint bits). Grid: fade origin xy, distance, major phase v.
    vec4 p0;         // Pipeline specific (brush: highlight; grid: minor phase uv, major phase u, spacing; sprites: right, w = outline; boxes: x = solid, y = outline).
    vec4 p1;         // Pipeline specific (grid: plane rect uMin/uMax/vMin/vMax; sprites: up, w = outline).
} push;

uint pushSlot() { return floatBitsToUint(push.offsetSlot.w); }
