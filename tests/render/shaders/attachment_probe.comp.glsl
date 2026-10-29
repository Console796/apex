#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#include "../../../src/render/shaders/heap.inc"

layout(local_size_x = 1) in;
layout(push_constant) uniform Probe { uvec2 outputAddress; uint depthSlot; } probe;
layout(buffer_reference, std430, buffer_reference_align = 4) writeonly buffer Output { float values[]; };

void main() {
    Output result = Output(probe.outputAddress);
    result.values[0] = textureLod(sampler2DShadow(textures[nonuniformEXT(probe.depthSlot)], samplers[2]),
                                  vec3(0.5, 0.5, 0.25), 0.0);
    result.values[1] = textureLod(sampler2DShadow(textures[nonuniformEXT(probe.depthSlot)], samplers[2]),
                                  vec3(0.5, 0.5, 0.75), 0.0);
    result.values[2] = 1.25;
    result.values[3] = -3.5;
    // Equal depth passes: GREATER_OR_EQUAL, not GREATER.
    result.values[4] = textureLod(sampler2DShadow(textures[nonuniformEXT(probe.depthSlot)], samplers[2]),
                                  vec3(0.5, 0.5, 0.5), 0.0);
}
