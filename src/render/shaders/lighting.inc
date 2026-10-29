#ifndef APEX_LIGHTING_INC
#define APEX_LIGHTING_INC
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// GpuLight in gpu_lighting.hpp; std430, 80-byte stride.
struct Light {
    vec4 originRadius;
    vec4 intensityExponent;
    vec4 directionInner;
    vec4 attenuationOuter;
    uvec4 kindMode;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LightBuffer {
    Light lights[];
};

float surfaceResponse(float cosine) { return cosine > 0.0 ? 0.5 * cosine * (1.0 + cosine) : 0.0; }

vec3 irradiance(Light light, vec3 point, vec3 normal) {
    if (light.kindMode.y == 0u) return vec3(0.0);
    uint kind = light.kindMode.x;
    if (kind == 3u) return light.intensityExponent.xyz * surfaceResponse(dot(normal, -light.directionInner.xyz));
    // D16 hook: ambient (kind 5) must wait for a sky-visibility gate; never fill sealed rooms.
    if (kind != 1u && kind != 2u) return vec3(0.0);
    vec3 delta = light.originRadius.xyz - point;
    float distance = length(delta);
    if (distance <= 0.0 || (light.originRadius.w > 0.0 && distance > light.originRadius.w)) return vec3(0.0);
    vec3 toLight = delta / distance;
    float cosine = dot(normal, toLight);
    if (cosine <= 0.0) return vec3(0.0);
    vec3 attn = light.attenuationOuter.xyz;
    float scale = surfaceResponse(cosine) / max(attn.x + attn.y * distance + attn.z * distance * distance, 1e-12);
    if (kind == 2u) {
        float along = dot(-toLight, light.directionInner.xyz);
        if (along <= light.attenuationOuter.w) return vec3(0.0);
        scale *= along;
        if (along < light.directionInner.w) {
            float edge = (along - light.attenuationOuter.w) / (light.directionInner.w - light.attenuationOuter.w);
            float exponent = light.intensityExponent.w;
            if (exponent != 0.0 && exponent != 1.0) edge = pow(edge, exponent);
            scale *= edge;
        }
    }
    return light.intensityExponent.xyz * scale;
}
#endif
