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

// UV change per world unit along the tangent frame's t and b: the lengths of the gradients of u and v.
vec2 uvPerUnit(vec3 normal, vec3 dpdx, vec3 dpdy, vec2 duvdx, vec2 duvdy) {
    vec3 px = cross(dpdy, normal), py = cross(normal, dpdx);
    float orientation = dot(dpdx, px);
    if (abs(orientation) <= 1e-20) return vec2(0.0);
    return vec2(length(px * duvdx.x + py * duvdy.x), length(px * duvdx.y + py * duvdy.y)) / abs(orientation);
}

vec3 perturbNormal(vec3 normal, vec3 dpdx, vec3 dpdy, vec2 duvdx, vec2 duvdy, vec3 tangentNormal) {
    normal = normalize(normal);
    vec3 t, b;
    if (!tangentFrame(normal, dpdx, dpdy, duvdx, duvdy, t, b)) return normal;
    vec3 result = t * tangentNormal.x + b * tangentNormal.y + normal * tangentNormal.z;
    return dot(result, result) > 1e-12 ? normalize(result) : normal;
}
#endif
