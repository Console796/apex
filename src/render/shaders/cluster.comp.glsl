#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "lighting.inc"
#include "clusters.inc"

layout(local_size_x = 64) in;

void main() {
    FrameBlock frame = frameBlock();
    uint index = gl_GlobalInvocationID.x;
    uvec3 dims = frame.clusterDims.xyz;
    if (index >= dims.x * dims.y * dims.z) return;
    uvec3 cell = uvec3(index % dims.x, (index / dims.x) % dims.y, index / (dims.x * dims.y));
    vec2 lo = vec2(cell.xy) / vec2(dims.xy) * 2.0 - 1.0;
    vec2 hi = vec2(cell.xy + 1u) / vec2(dims.xy) * 2.0 - 1.0;
    float nearDepth = exp2(-float(cell.z));
    float farDepth = cell.z + 1u == dims.z ? 0.0 : nearDepth * 0.5;
    mat4 rows = transpose(frame.clusterProjection);
    vec4 planes[6] = vec4[6](rows[0] - lo.x * rows[3], hi.x * rows[3] - rows[0],
        rows[1] - lo.y * rows[3], hi.y * rows[3] - rows[1],
        rows[2] - farDepth * rows[3], nearDepth * rows[3] - rows[2]);
    uint count = 0u;
    for (uint i = frame.clusterDims.w; i < frame.lightCount; ++i) {
        Light light = LightBuffer(frame.lights).lights[i];
        if ((!hasDirect(light) && !hasSpecular(light)) || light.originRadius.w <= 0.0) continue;
        bool intersects = true;
        if (!isinf(light.originRadius.w)) for (uint p = 0u; p < 6u; ++p)
            if (dot(planes[p], vec4(light.originRadius.xyz, 1.0)) <
                -light.originRadius.w * length(planes[p].xyz)) intersects = false;
        if (!intersects) continue;
        if (count < CLUSTER_CAPACITY) ClusterBuffer(frame.clusters).cells[index].indices[count] = i;
        ++count;
    }
    ClusterBuffer(frame.clusters).cells[index].count = min(count, CLUSTER_CAPACITY);
    if (count > CLUSTER_CAPACITY) atomicAdd(ClusterStats(frame.clusterStats).overflow, count - CLUSTER_CAPACITY);
}
