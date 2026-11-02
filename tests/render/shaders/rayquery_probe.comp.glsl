#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(local_size_x = 1) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;

struct Ray { vec4 origin; vec4 direction; uvec4 options; };
struct Hit { uint hit; float distance; uint customIndex; uint padding; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Rays { Ray values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer Hits { Hit values[]; };
layout(push_constant) uniform Probe { uvec2 rays; uvec2 hits; } probe;

void main() {
    uint i = gl_GlobalInvocationID.x;
    Ray ray = Rays(probe.rays).values[i];
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, gl_RayFlagsOpaqueEXT, ray.options.x,
                         ray.origin.xyz, ray.origin.w, ray.direction.xyz, ray.direction.w);
    while (rayQueryProceedEXT(query)) {}
    bool hit = rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
    Hits(probe.hits).values[i] = Hit(hit ? 1u : 0u,
        hit ? rayQueryGetIntersectionTEXT(query, true) : -1.0,
        hit ? rayQueryGetIntersectionInstanceCustomIndexEXT(query, true) : 0xFFFFFFFFu, 0u);
}
