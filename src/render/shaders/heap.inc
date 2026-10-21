#extension GL_EXT_nonuniform_qualifier : require
// Bindless texture heap (render/texture_heap.hpp), set 0.
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler samplers[2]; // 0 linear repeat, 1 nearest repeat

vec4 sampleSlot(uint slot, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(slot)], samplers[0]), uv);
}
