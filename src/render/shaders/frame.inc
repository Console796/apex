#ifndef APEX_FRAME_INC
#define APEX_FRAME_INC
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// FrameBlock in gpu_lighting.hpp; 64 bytes, one allocation per recording slot.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameBlock {
    uvec2 previews;
    uvec2 lights;
    uint lightCount;
    uint flags;
    uvec2 pad;
    vec4 cameraRight;
    vec4 cameraUp;
};
FrameBlock frameBlock() { return FrameBlock(floatBitsToUint(push.p1.xy)); }
#endif
