#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#define APEX_FRAME_NO_PUSH
#include "lighting.inc"
#include "probes.inc"

layout(local_size_x = 128) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;
struct Triangle { vec4 a; vec4 b; vec4 c; vec4 albedo; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Triangles { Triangle values[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Offsets { uint values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer ProbeOutput { IrradianceProbe probes[]; };
// Shared cascade context (CascadeContext in probe_volume.cpp): the previous-frame grids of both
// cascades, used by every lane to transport bounce. The push constants' dims/originSpacing/params
// always describe the cascade currently being updated.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer CascadeContext {
    uvec4 fineDims;
    vec4 fineOriginSpacing;
    vec4 fineParams;      // x: fine bias; y: fine fade cells.
    uvec4 addresses;      // xy: fine previous; zw: coarse previous.
    uvec4 coarseDims;
    vec4 coarseOriginSpacing;
    vec4 coarseParams;    // x: coarse bias; y: relocate enabled; z: clearance fraction; w: max fraction.
};
layout(push_constant) uniform Update {
    uvec2 previous;
    uvec2 outputMaps;
    uvec2 triangles;
    uvec2 offsets;
    uvec2 lights;
    uint lightCount;
    uint first;
    uvec4 dims;
    vec4 originSpacing;
    vec4 params; // hysteresis, normal bias, rotation, max distance
    uvec2 context;
} push;
shared vec4 radianceDistance[128];
shared vec4 directionsBackface[128];
shared float skyRay[128];
shared vec3 relocationOffset;

bool blocked(vec3 point, vec3 direction, float distance, bool allowSky) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, gl_RayFlagsOpaqueEXT,
                           0xFF, point, 0.05, direction, distance);
    while (rayQueryProceedEXT(q)) {}
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) return false;
    uint instance = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true);
    uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
    return !allowSky || Triangles(push.triangles).values[Offsets(push.offsets).values[instance] + primitive].albedo.w != 1.0;
}
vec3 sky(vec3 direction) {
    if (direction.z <= 0.0) return vec3(0.0);
    vec3 value = vec3(0.0);
    for (uint i = 0u; i < push.lightCount; ++i) {
        Light light = LightBuffer(push.lights).lights[i];
        // Static (1) and Dynamic (2) both inject: the preview shows what the bake will, and VRAD bounces
        // Static lights. Only None (0) stays out.
        if (light.kindMode.x == 5u && light.kindMode.w != 0u)
            value += light.intensityExponent.rgb * (6.0 / (5.0 * 3.14159265359));
    }
    return value;
}
// Bounce energy at a ray hit, sampled from both previous-frame cascades exactly like the
// runtime sampler, so far-field hits feed the updating probe instead of going dark.
vec3 transported(vec3 point, vec3 normal) {
    CascadeContext context = CascadeContext(push.context);
    return cascadeIrradiance(context.addresses.xy,
                             uvec3(context.fineDims.xyz), context.fineOriginSpacing.xyz,
                             context.fineOriginSpacing.w, context.fineParams.x, context.fineParams.y,
                             context.addresses.zw, uvec3(context.coarseDims.xyz), context.coarseOriginSpacing.xyz,
                             context.coarseOriginSpacing.w, context.coarseParams.x,
                             point, normal);
}
// Deterministic relocation: a probe that exits a solid is pushed out along the first (fixed
// axis order) escape normal; a probe too close to a wall moves back out to the clearance
// radius. The offset is bounded by the settings' max fraction of the spacing.
vec3 relocate(vec3 gridPoint) {
    CascadeContext context = CascadeContext(push.context);
    if (context.coarseParams.y < 0.5) return vec3(0.0);
    float radius = context.coarseParams.z * push.originSpacing.w;
    float maxOffset = context.coarseParams.w * push.originSpacing.w;
    const vec3 axes[6] = vec3[](vec3(-1, 0, 0), vec3(1, 0, 0), vec3(0, -1, 0),
                                vec3(0, 1, 0), vec3(0, 0, -1), vec3(0, 0, 1));
    // Nearest exit (a ray that meets a back face) wins; ties keep the first axis in the fixed
    // order, so the result is deterministic. Without any exit, the nearest front face closer than
    // the clearance radius pushes the probe away from that wall instead.
    float exitDistance = 1e30, wallDistance = 1e30;
    vec3 exitNormal = vec3(0.0), wallNormal = vec3(0.0);
    rayQueryEXT q;
    for (uint i = 0u; i < 6u; ++i) {
        rayQueryInitializeEXT(q, scene, gl_RayFlagsOpaqueEXT, 0xFF, gridPoint, 0.0,
                              axes[i], maxOffset + radius);
        while (rayQueryProceedEXT(q)) {}
        if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) continue;
        float distance = rayQueryGetIntersectionTEXT(q, true);
        uint instance = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true);
        uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
        Triangle triangle = Triangles(push.triangles).values[Offsets(push.offsets).values[instance] + primitive];
        vec3 normal = normalize(cross(triangle.b.xyz - triangle.a.xyz, triangle.c.xyz - triangle.a.xyz));
        if (dot(normal, axes[i]) > 0.0) { // Back face: the probe sits inside a solid.
            if (distance < exitDistance) { exitDistance = distance; exitNormal = normal; }
        } else if (distance < wallDistance && distance < radius) {
            wallDistance = distance; wallNormal = normal;
        }
    }
    if (exitDistance < 1e30) return exitNormal * min(maxOffset, exitDistance + radius);
    if (wallDistance < 1e30) return wallNormal * min(maxOffset, radius - wallDistance);
    return vec3(0.0);
}
void main() {
    uint lane = gl_LocalInvocationID.x;
    uint count = push.dims.x * push.dims.y * push.dims.z;
    uint index = (push.first + gl_WorkGroupID.x) % count;
    uvec3 cell = uvec3(index % push.dims.x, (index / push.dims.x) % push.dims.y,
                       index / (push.dims.x * push.dims.y));
    vec3 gridPoint = push.originSpacing.xyz + vec3(cell) * push.originSpacing.w;
    if (lane == 0u) relocationOffset = relocate(gridPoint) / push.originSpacing.w;
    barrier();
    vec3 point = gridPoint + push.originSpacing.w * relocationOffset;
    float z = 1.0 - 2.0 * (float(lane) + 0.5) / 128.0;
    float phi = 6.28318530718 * (float(lane) * 0.61803398875 + push.params.z);
    vec3 direction = vec3(sqrt(1.0 - z * z) * vec2(cos(phi), sin(phi)), z);
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, gl_RayFlagsOpaqueEXT, 0xFF, point, 0.05, direction, push.params.w);
    while (rayQueryProceedEXT(q)) {}
    float distance = push.params.w, backface = 0.0;
    float fromSky = 1.0;
    vec3 value = sky(direction);
    if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
        distance = rayQueryGetIntersectionTEXT(q, true);
        uint instance = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true);
        uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
        Triangle triangle = Triangles(push.triangles).values[Offsets(push.offsets).values[instance] + primitive];
        fromSky = triangle.albedo.w == 1.0 ? 1.0 : 0.0;
        vec3 normal = normalize(cross(triangle.b.xyz - triangle.a.xyz, triangle.c.xyz - triangle.a.xyz));
        backface = dot(normal, direction) > 0.0 ? 1.0 : 0.0;
        value = vec3(0.0);
        if (triangle.albedo.w == 1.0) value = sky(direction);
        else if (backface == 0.0 && triangle.albedo.w == 0.0) {
            vec3 hit = point + direction * distance;
            vec3 energy = transported(hit, normal);
            for (uint i = 0u; i < push.lightCount; ++i) {
                Light light = LightBuffer(push.lights).lights[i];
                if (light.kindMode.w == 0u || light.kindMode.x == 5u) continue;
                vec3 toLight = light.kindMode.x == 3u ? -light.directionInner.xyz : light.originRadius.xyz - hit;
                float lengthToLight = length(toLight);
                if (lengthToLight < 0.1) continue;
                vec3 direct = localIrradiance(light, hit, normal);
                if (any(greaterThan(direct, vec3(0.0))) && !blocked(hit + normal * 0.1,
                    toLight / lengthToLight, light.kindMode.x == 3u ? push.params.w : max(lengthToLight - 0.2, 0.05), light.kindMode.x == 3u))
                    energy += direct;
            }
            value = energy * triangle.albedo.rgb / 3.14159265359;
        }
    }
    radianceDistance[lane] = vec4(value, min(distance, push.originSpacing.w * 4.0));
    directionsBackface[lane] = vec4(direction, backface);
    skyRay[lane] = fromSky;
    barrier();
    if (lane >= 64u) return;
    float backfaces = 0.0;
    vec3 axis = octDecode((vec2(lane % 8u, lane / 8u) + 0.5) / 8.0);
    vec3 sum = vec3(0.0);
    vec2 moments = vec2(0.0);
    float weight = 0.0;
    for (uint r = 0u; r < 128u; ++r) {
        float cosine = max(dot(axis, directionsBackface[r].xyz), 0.0);
        // Surface bounce uses Lambertian transport, matching the CPU baker.
        // Keep the previously calibrated angular response for the sky emitter.
        sum += radianceDistance[r].rgb * mix(cosine, surfaceResponse(cosine), skyRay[r]);
        float w = pow(cosine, 64.0);
        float d = radianceDistance[r].w;
        moments += w * vec2(d, d * d);
        weight += w;
        backfaces += directionsBackface[r].w;
    }
    sum *= 12.56637061436 / 128.0;
    moments /= max(weight, 1e-8);
    ProbeBuffer previous = ProbeBuffer(push.previous);
    bool valid = backfaces < 32.0;
    float blend = previous.probes[index].irradiance[0].w > 0.0 && valid ? push.params.x : 0.0;
    ProbeOutput outMaps = ProbeOutput(push.outputMaps);
    outMaps.probes[index].irradiance[lane] =
        vec4(mix(sum, previous.probes[index].irradiance[lane].rgb, blend), valid ? 1.0 : -1.0);
    // Texel 0/1's unused depth channels carry the relocation offset for the sampler.
    if (lane == 0u) outMaps.probes[index].depth[0] =
        vec4(mix(moments, previous.probes[index].depth[0].xy, blend), relocationOffset.xy);
    else if (lane == 1u) outMaps.probes[index].depth[1] =
        vec4(mix(moments, previous.probes[index].depth[1].xy, blend), relocationOffset.z, 0.0);
    else outMaps.probes[index].depth[lane] =
        vec4(mix(moments, previous.probes[index].depth[lane].xy, blend), 0.0, 0.0);
}
