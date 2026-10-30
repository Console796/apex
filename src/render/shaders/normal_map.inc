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

// Unit tangent (+U) and bitangent (+V) from screen-space derivatives; false when degenerate.
bool tangentFrame(vec3 normal, vec3 dpdx, vec3 dpdy, vec2 duvdx, vec2 duvdy, out vec3 t, out vec3 b) {
    vec3 px = cross(dpdy, normal), py = cross(normal, dpdx);
    float orientation = dot(dpdx, px);
    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    t = vec3(1.0, 0.0, 0.0);
    b = vec3(0.0, 1.0, 0.0);
    if (orientation == 0.0 || det * det <= 1e-12 * dot(duvdx, duvdx) * dot(duvdy, duvdy)) return false;
    vec3 tu = (px * duvdx.x + py * duvdy.x) * sign(orientation);
    vec3 bv = (px * duvdx.y + py * duvdy.y) * sign(orientation);
    if (dot(tu, tu) <= 1e-20 || dot(bv, bv) <= 1e-20) return false;
    t = normalize(tu);
    b = normalize(bv);
    return true;
}

vec3 perturbNormal(vec3 normal, vec3 dpdx, vec3 dpdy, vec2 duvdx, vec2 duvdy, vec3 tangentNormal) {
    normal = normalize(normal);
    vec3 t, b;
    if (!tangentFrame(normal, dpdx, dpdy, duvdx, duvdy, t, b)) return normal;
    vec3 result = t * tangentNormal.x + b * tangentNormal.y + normal * tangentNormal.z;
    return dot(result, result) > 1e-12 ? normalize(result) : normal;
}
#endif
