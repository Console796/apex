// Vertex-only BDA access. uvec2 addresses avoid requiring shaderInt64. Each entry is the
// 48-byte PreviewTransform in view_math.hpp, already rebased for its draw origin and camera.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

struct PreviewTransform { vec4 rows[3]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PreviewBuffer {
    PreviewTransform transforms[];
};

#ifdef PREVIEW_SPRITE
uint previewIndex() { return floatBitsToUint(push.p0.w) >> 1; }
uvec2 previewAddress() { return uvec2(floatBitsToUint(push.offsetSlot.w), floatBitsToUint(push.p1.w)); }
#else
uint previewIndex() {
    uint index = floatBitsToUint(push.p1.z);
    return (index & 0x80000000u) != 0u ? (index & 0x7fffffffu) + gl_InstanceIndex : index;
}
uvec2 previewAddress() { return floatBitsToUint(push.p1.xy); }
#endif

vec3 previewPosition(vec3 local) {
    uint index = previewIndex();
    if (index == 0) return local + push.offsetSlot.xyz;
    PreviewTransform t = PreviewBuffer(previewAddress()).transforms[index - 1];
    vec4 p = vec4(local, 1.0);
    return vec3(dot(t.rows[0], p), dot(t.rows[1], p), dot(t.rows[2], p));
}

vec3 previewNormal(vec3 normal) {
    uint index = previewIndex();
    if (index == 0) return normal;
    PreviewTransform t = PreviewBuffer(previewAddress()).transforms[index - 1];
    mat3 linear = transpose(mat3(t.rows[0].xyz, t.rows[1].xyz, t.rows[2].xyz));
    return normalize(transpose(inverse(linear)) * normal);
}
