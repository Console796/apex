#ifndef APEX_FRAME_INC
#define APEX_FRAME_INC
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// FrameBlock in gpu_lighting.hpp; 160 bytes, one allocation per recording slot.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameBlock {
    uvec2 previews;
    uvec2 lights;
    uint lightCount;
    uint flags;
    uvec2 pad;
    vec4 cameraRight;
    vec4 cameraUp;
    uvec2 skyProbes;
    uvec2 skyPad;
    uvec4 skyDims;
    vec4 skyOrigin;
    vec4 skyCellSize;
    uvec2 materials;
    uint materialCount;
    uint materialPad;
    uvec2 cubemaps;
    uint cubemapCount;
    uint cubemapPad;
};
#ifndef APEX_FRAME_NO_PUSH
FrameBlock frameBlock() { return FrameBlock(floatBitsToUint(push.p1.xy)); }
#endif
#endif
