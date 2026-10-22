// Shared by every shader (plan 0.1.9). Push constants are 128 bytes, identical for all pipelines
// (mirrors PushConstants in render/renderer.hpp).
layout(push_constant) uniform Push {
    mat4 viewProj;   // Rotation-only view * projection; positions are camera-relative.
    vec4 colour;
    vec4 offsetSlot; // xyz: draw origin relative to the camera; w: texture slot (uint bits).
    vec4 p0;         // Pipeline specific (brush: highlight; grid: cameraLo + spacing; sprites: right, w = outline; boxes: x = solid, y = outline).
    vec4 p1;         // Pipeline specific (grid: x = fade distance, y = plane z rel; sprites: up, w = outline).
} push;

uint pushSlot() { return floatBitsToUint(push.offsetSlot.w); }
