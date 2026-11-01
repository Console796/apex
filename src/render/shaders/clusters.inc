#ifndef APEX_CLUSTERS_INC
#define APEX_CLUSTERS_INC
#include "frame.inc"

const uint CLUSTER_CAPACITY = 64u;
struct Cluster { uint count; uint indices[64]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer ClusterBuffer { Cluster cells[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer ClusterStats { uint overflow; };

uint clusterIndex(FrameBlock frame, vec3 point) {
    vec4 clip = frame.clusterProjection * vec4(point, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    uvec2 xy = uvec2(clamp((ndc.xy * 0.5 + 0.5) * vec2(frame.clusterDims.xy),
                           vec2(0.0), vec2(frame.clusterDims.xy - 1u)));
    uint z = uint(clamp(floor(-log2(max(ndc.z, 1e-30))), 0.0, float(frame.clusterDims.z - 1u)));
    return (z * frame.clusterDims.y + xy.y) * frame.clusterDims.x + xy.x;
}
#endif
