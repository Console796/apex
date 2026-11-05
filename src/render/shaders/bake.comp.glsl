#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_ray_query : require

layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;

struct Face {
    vec4 normal;
    vec4 albedo;
    vec4 chartOrigin;
    vec4 luxelToWorld[2];
    vec4 worldToLuxel[2];
    vec4 offsetMins;
    uvec4 dimensionsVertices;
    uvec4 source;
};
struct Light {
    vec4 originRadius;
    vec4 intensity;
    vec4 directionStopdot;
    vec4 shadowOffsetStopdot2;
    vec4 attenuationExponent;
    uvec4 kindDirect;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Faces { Face values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Vertices { vec4 values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Lights { Light values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Primitives { uvec4 values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Work { uvec4 values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Source { vec4 values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer Output { vec4 values[]; };

layout(push_constant) uniform Bake {
    uvec2 faces;
    uvec2 vertices;
    uvec2 lights;
    uvec2 primitives;
    uvec2 work;
    uvec2 source;
    uvec2 outputValues;
    uint first;
    uint workCount;
    uint lightCount;
    uint skyRays;
    uint bounceRays;
    uint level;
    uint seedLow;
    uint seedHigh;
    uint hasGeometry;
    float bounceReflectivity;
} push;

const float pi = 3.14159265358979323846;
const uint noFace = 0xffffffffu;

uint hash(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
uint combine(uint value, uint next) { return hash(value ^ hash(next + 0x9e3779b9u)); }
float unit(uint value) { return float(hash(value) >> 8) * (1.0 / 16777216.0); }
uint sampleKey(uvec4 item, uint corner, uint level) {
    uint key = combine(push.seedLow, push.seedHigh);
    key = combine(key, item.x);
    key = combine(key, item.y);
    key = combine(key, level);
    key = combine(key, item.z);
    key = combine(key, item.w);
    return combine(key, corner + 1u);
}
float surfaceResponse(float cosine) { return cosine > 0.0 ? 0.5 * cosine * (1.0 + cosine) : 0.0; }
float skyAmbientResponse(float normalZ) {
    float z = clamp(normalZ, -1.0, 1.0);
    return 0.3 * (1.0 + z) + 0.2 + 2.0 / (5.0 * pi) * (asin(z) + z * sqrt(max(0.0, 1.0 - z * z)));
}

vec3 clampToFace(Face face, vec3 point) {
    uint count = face.dimensionsVertices.w;
    if (count < 3u) return point;
    vec3 centre = vec3(0.0);
    for (uint i = 0u; i < count; ++i)
        centre += Vertices(push.vertices).values[face.dimensionsVertices.z + i].xyz;
    centre /= float(count);
    bool inside = true;
    float best = 3.402823466e38;
    vec3 nearest = centre;
    for (uint i = 0u; i < count; ++i) {
        vec3 a = Vertices(push.vertices).values[face.dimensionsVertices.z + i].xyz;
        vec3 b = Vertices(push.vertices).values[face.dimensionsVertices.z + (i + 1u) % count].xyz;
        vec3 edge = b - a;
        float length2 = dot(edge, edge);
        if (length2 <= 0.0) continue;
        vec3 inward = cross(face.normal.xyz, edge);
        if (dot(inward, centre - a) < 0.0) inward = -inward;
        if (dot(inward, point - a) < sqrt(dot(inward, inward))) inside = false;
        vec3 closest = a + edge * clamp(dot(point - a, edge) / length2, 0.0, 1.0);
        float distance = length(point - closest);
        if (distance < best) { best = distance; nearest = closest; }
    }
    if (inside) return point;
    vec3 toCentre = centre - nearest;
    float distance = length(toCentre);
    return distance > 0.0 ? nearest + toCentre * min(1.0, 1.0 / distance) : centre;
}

bool trace(vec3 point, vec3 direction, float distance, out uvec4 primitive, out float hitDistance) {
    if (push.hasGeometry == 0u) return false;
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, gl_RayFlagsOpaqueEXT, 0xff, point, 0.0, direction, distance);
    while (rayQueryProceedEXT(query)) {}
    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT) return false;
    primitive = Primitives(push.primitives).values[rayQueryGetIntersectionPrimitiveIndexEXT(query, true)];
    hitDistance = rayQueryGetIntersectionTEXT(query, true);
    return true;
}
bool reachesSky(vec3 point, vec3 direction) {
    uvec4 primitive; float distance;
    return !trace(point, direction, 3.402823466e38, primitive, distance) || primitive.x != 0u;
}
bool visible(vec3 point, vec3 endpoint) {
    vec3 delta = endpoint - point;
    float distance = length(delta);
    if (push.hasGeometry == 0u || distance <= 0.0) return true;
    float reach = uintBitsToFloat(floatBitsToUint(distance) - 1u);
    uvec4 primitive; float hitDistance;
    return !trace(point, delta / distance, reach, primitive, hitDistance);
}

vec3 irradiance(Light light, vec3 point, vec3 normal) {
    uint kind = light.kindDirect.x;
    if (kind == 3u) return light.intensity.rgb * surfaceResponse(dot(normal, -light.directionStopdot.xyz));
    if (kind == 5u) return light.intensity.rgb * skyAmbientResponse(normal.z);
    vec3 delta = light.originRadius.xyz - point;
    float distance = length(delta);
    if (distance <= 0.0 || (light.originRadius.w > 0.0 && distance > light.originRadius.w)) return vec3(0.0);
    vec3 toLight = delta / distance;
    float cosine = dot(normal, toLight);
    if (cosine <= 0.0) return vec3(0.0);
    vec3 attenuation = light.attenuationExponent.xyz;
    float scale = surfaceResponse(cosine) /
        max(attenuation.x + attenuation.y * distance + attenuation.z * distance * distance, 1e-12);
    if (kind == 2u) {
        float along = dot(-toLight, light.directionStopdot.xyz);
        if (along <= light.shadowOffsetStopdot2.w) return vec3(0.0);
        scale *= along;
        if (along < light.directionStopdot.w) {
            float edge = (along - light.shadowOffsetStopdot2.w) /
                         (light.directionStopdot.w - light.shadowOffsetStopdot2.w);
            float exponent = light.attenuationExponent.w;
            if (exponent != 0.0 && exponent != 1.0) edge = pow(edge, exponent);
            scale *= edge;
        }
    }
    return light.intensity.rgb * scale;
}

float skyVisibility(vec3 point, vec3 normal, uint key) {
    float visibleWeight = 0.0, openWeight = 0.0;
    float rotation = unit(key);
    for (uint ray = 0u; ray < push.skyRays; ++ray) {
        float z = (float(ray) + unit(key ^ hash(ray + 1u))) / float(push.skyRays);
        float radius = sqrt(max(0.0, 1.0 - z * z));
        float phi = 2.0 * pi * (rotation + float(ray) * 0.6180339887498949);
        vec3 direction = vec3(radius * cos(phi), radius * sin(phi), z);
        float weight = surfaceResponse(dot(normal, direction));
        openWeight += weight;
        if (weight > 0.0 && reachesSky(point, direction)) visibleWeight += weight;
    }
    return openWeight > 0.0 ? visibleWeight / openWeight : 0.0;
}

vec3 directSample(Face face, uvec4 item) {
    const vec2 offsets[4] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5),
                                   vec2(-0.5, 0.5), vec2(0.5, 0.5));
    uint width = face.dimensionsVertices.x;
    vec2 coordinate = vec2(item.y % width, item.y / width);
    vec3 sum = vec3(0.0);
    for (uint corner = 0u; corner < 4u; ++corner) {
        vec2 local = coordinate + offsets[corner];
        vec3 point = clampToFace(face, face.chartOrigin.xyz + local.x * face.luxelToWorld[0].xyz +
                                      local.y * face.luxelToWorld[1].xyz) + face.normal.xyz;
        float sky = -1.0;
        for (uint index = 0u; index < push.lightCount; ++index) {
            Light light = Lights(push.lights).values[index];
            if (light.kindDirect.y != 1u && light.kindDirect.y != 2u) continue;
            vec3 value = irradiance(light, point, face.normal.xyz);
            if (all(equal(value, vec3(0.0)))) continue;
            if (light.kindDirect.x == 5u) {
                if (sky < 0.0) sky = skyVisibility(point, face.normal.xyz, sampleKey(item, corner, 0u));
                sum += value * sky;
            } else if (light.kindDirect.x == 3u) {
                if (reachesSky(point, -light.directionStopdot.xyz)) sum += value;
            } else if (visible(point, light.originRadius.xyz + light.shadowOffsetStopdot2.xyz)) {
                sum += value;
            }
        }
    }
    return sum;
}

vec3 bounceSample(Face face, uvec4 item) {
    if (push.hasGeometry == 0u) return vec3(0.0);
    const vec2 offsets[4] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5),
                                   vec2(-0.5, 0.5), vec2(0.5, 0.5));
    uint width = face.dimensionsVertices.x;
    vec2 coordinate = vec2(item.y % width, item.y / width);
    vec3 axis = abs(face.normal.z) < 0.9 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    vec3 tangent = normalize(cross(axis, face.normal.xyz));
    vec3 bitangent = cross(face.normal.xyz, tangent);
    vec3 sum = vec3(0.0);
    for (uint corner = 0u; corner < 4u; ++corner) {
        vec2 local = coordinate + offsets[corner];
        vec3 point = clampToFace(face, face.chartOrigin.xyz + local.x * face.luxelToWorld[0].xyz +
                                      local.y * face.luxelToWorld[1].xyz) + face.normal.xyz;
        uint key = sampleKey(item, corner, push.level);
        float rotation = unit(key);
        for (uint ray = 0u; ray < push.bounceRays; ++ray) {
            float u = (float(ray) + unit(key ^ hash(ray + 1u))) / float(push.bounceRays);
            float radius = sqrt(u);
            float phi = 2.0 * pi * (rotation + float(ray) * 0.6180339887498949);
            vec3 direction = tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi)) +
                             face.normal.xyz * sqrt(max(0.0, 1.0 - u));
            uvec4 primitive; float distance;
            if (!trace(point, direction, 3.402823466e38, primitive, distance) || primitive.x != 0u || primitive.y == noFace)
                continue;
            Face hitFace = Faces(push.faces).values[primitive.y];
            if (dot(hitFace.normal.xyz, direction) >= 0.0) continue;
            vec3 position = point + direction * distance;
            ivec2 st;
            for (int chartAxis = 0; chartAxis < 2; ++chartAxis) {
                float chart = dot(position, hitFace.worldToLuxel[chartAxis].xyz) +
                              hitFace.offsetMins[chartAxis] - hitFace.offsetMins[chartAxis + 2];
                st[chartAxis] = int(round(clamp(chart, 0.0, float(hitFace.dimensionsVertices[chartAxis] - 1u))));
            }
            uint sampleIndex = hitFace.source.x + uint(st.y) * hitFace.dimensionsVertices.x + uint(st.x);
            sum += Source(push.source).values[sampleIndex].rgb * hitFace.albedo.rgb *
                   (push.bounceReflectivity / float(push.bounceRays));
        }
    }
    return sum;
}

void main() {
    uint outputIndex = push.first + gl_GlobalInvocationID.x;
    if (outputIndex >= push.workCount) return;
    uvec4 item = Work(push.work).values[outputIndex];
    Face face = Faces(push.faces).values[item.x];
    vec3 value = push.level == 0u ? directSample(face, item) : bounceSample(face, item);
    Output(push.outputValues).values[outputIndex] = vec4(value, 0.0);
}
