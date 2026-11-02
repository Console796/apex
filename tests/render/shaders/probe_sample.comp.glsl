#version 450
#extension GL_GOOGLE_include_directive : require
#define APEX_FRAME_NO_PUSH
#include "../../../src/render/shaders/lighting.inc"
layout(local_size_x = 1) in;
struct Sample { vec4 point; vec4 normal; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Samples { Sample values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer Output { vec4 values[]; };
layout(push_constant) uniform Push { uvec2 frame; uvec2 inputAddress; uvec2 outputAddress; } push;
void main() {
    uint i = gl_GlobalInvocationID.x;
    Sample s = Samples(push.inputAddress).values[i];
    Output(push.outputAddress).values[i] = vec4(sampleIndirect(FrameBlock(push.frame), s.point.xyz, s.normal.xyz), 0.0);
}
