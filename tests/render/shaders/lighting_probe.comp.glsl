#version 450
#extension GL_GOOGLE_include_directive : require
#include "../../../src/render/shaders/lighting.inc"

layout(local_size_x = 1) in;
struct Sample { Light light; vec4 point; vec4 normal; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Samples { Sample samples[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer Output { vec4 values[]; };
layout(push_constant) uniform Probe { uvec2 inputAddress; uvec2 outputAddress; } probe;

void main() {
    uint i = gl_GlobalInvocationID.x;
    Sample s = Samples(probe.inputAddress).samples[i];
    Output(probe.outputAddress).values[i] = vec4(irradiance(s.light, s.point.xyz, s.normal.xyz), 0.0);
}
