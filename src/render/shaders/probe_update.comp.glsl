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
} push;
shared vec4 radianceDistance[128];
shared vec4 directionsBackface[128];

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
        if (light.kindMode.x == 5u && light.kindMode.w == 2u)
            value += light.intensityExponent.rgb * (6.0 / (5.0 * 3.14159265359));
    }
    return value;
}
void main() {
    uint lane = gl_LocalInvocationID.x;
    uint count = push.dims.x * push.dims.y * push.dims.z;
    uint index = (push.first + gl_WorkGroupID.x) % count;
    uvec3 cell = uvec3(index % push.dims.x, (index / push.dims.x) % push.dims.y,
                       index / (push.dims.x * push.dims.y));
    vec3 point = push.originSpacing.xyz + vec3(cell) * push.originSpacing.w;
    float z = 1.0 - 2.0 * (float(lane) + 0.5) / 128.0;
    float phi = 6.28318530718 * (float(lane) * 0.61803398875 + push.params.z);
    vec3 direction = vec3(sqrt(1.0 - z * z) * vec2(cos(phi), sin(phi)), z);
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, gl_RayFlagsOpaqueEXT, 0xFF, point, 0.05, direction, push.params.w);
    while (rayQueryProceedEXT(q)) {}
    float distance = push.params.w, backface = 0.0;
    vec3 value = sky(direction);
    if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
        distance = rayQueryGetIntersectionTEXT(q, true);
        uint instance = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true);
        uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
        Triangle triangle = Triangles(push.triangles).values[Offsets(push.offsets).values[instance] + primitive];
        vec3 normal = normalize(cross(triangle.b.xyz - triangle.a.xyz, triangle.c.xyz - triangle.a.xyz));
        backface = dot(normal, direction) > 0.0 ? 1.0 : 0.0;
        value = vec3(0.0);
        if (triangle.albedo.w == 1.0) value = sky(direction);
        else if (backface == 0.0 && triangle.albedo.w == 0.0) {
            vec3 hit = point + direction * distance;
            vec3 energy = probeIrradiance(push.previous, push.dims.xyz, push.originSpacing.xyz,
                push.originSpacing.w, push.params.y, hit, normal);
            for (uint i = 0u; i < push.lightCount; ++i) {
                Light light = LightBuffer(push.lights).lights[i];
                if (light.kindMode.w != 2u || light.kindMode.x == 5u) continue;
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
    barrier();
    if (lane >= 64u) return;
    float backfaces = 0.0;
    vec3 axis = octDecode((vec2(lane % 8u, lane / 8u) + 0.5) / 8.0);
    vec3 sum = vec3(0.0);
    vec2 moments = vec2(0.0);
    float weight = 0.0;
    for (uint r = 0u; r < 128u; ++r) {
        float cosine = max(dot(axis, directionsBackface[r].xyz), 0.0);
        sum += radianceDistance[r].rgb * surfaceResponse(cosine);
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
    ProbeOutput(push.outputMaps).probes[index].irradiance[lane] =
        vec4(mix(sum, previous.probes[index].irradiance[lane].rgb, blend), valid ? 1.0 : -1.0);
    ProbeOutput(push.outputMaps).probes[index].depth[lane] =
        vec4(mix(moments, previous.probes[index].depth[lane].xy, blend), 0.0, 0.0);
}
