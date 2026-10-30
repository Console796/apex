#ifndef APEX_NORMAL_MAP_INC
#define APEX_NORMAL_MAP_INC

vec3 decodeNormal(vec3 texel, bool ssbump, bool twoChannel) {
    // VDC $ssbump; see probe-results/material_normals_0_6_5.md. RGB normals use +V green.
    vec3 normal = ssbump ?
        texel.r * vec3(0.816496580927726, 0.0, 0.577350269189626) +
        texel.g * vec3(-0.408248290463863, 0.707106781186548, 0.577350269189626) +
        texel.b * vec3(-0.408248290463863, -0.707106781186548, 0.577350269189626) :
        texel * 2.0 - 1.0;
    if (!ssbump && twoChannel) normal.z = sqrt(max(0.0, 1.0 - dot(normal.xy, normal.xy)));
    return dot(normal, normal) > 1e-12 ? normalize(normal) : vec3(0.0, 0.0, 1.0);
}

vec3 perturbNormal(vec3 normal, vec3 dpdx, vec3 dpdy, vec2 duvdx, vec2 duvdy, vec3 tangentNormal) {
    normal = normalize(normal);
    vec3 px = cross(dpdy, normal), py = cross(normal, dpdx);
    float orientation = dot(dpdx, px);
    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (orientation == 0.0 || det * det <= 1e-12 * dot(duvdx, duvdx) * dot(duvdy, duvdy)) return normal;
    vec3 t = (px * duvdx.x + py * duvdy.x) * sign(orientation);
    vec3 b = (px * duvdx.y + py * duvdy.y) * sign(orientation);
    if (dot(t, t) <= 1e-20 || dot(b, b) <= 1e-20) return normal;
    vec3 result = normalize(t) * tangentNormal.x + normalize(b) * tangentNormal.y + normal * tangentNormal.z;
    return dot(result, result) > 1e-12 ? normalize(result) : normal;
}
#endif
