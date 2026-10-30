#ifndef APEX_SPECULAR_INC
#define APEX_SPECULAR_INC

// Provisional CPU transcription: lighting/specular.hpp. Karis, Real Shading in Unreal
// Engine 4 (SIGGRAPH 2013), eqs. 2-4, 8. No Valve/Strata shader source used.
vec3 specularF0(vec3 albedo, float metalness) {
    return mix(vec3(0.04), albedo, clamp(metalness, 0.0, 1.0));
}
vec3 schlickFresnel(vec3 f0, float viewHalf) {
    float x = 1.0 - clamp(viewHalf, 0.0, 1.0);
    return f0 + (vec3(1.0) - f0) * (x * x * x * x * x);
}
vec3 ggxBrdf(vec3 normal, vec3 view, vec3 toLight, vec3 albedo, float metalness, float roughness) {
    float nl = dot(normal, toLight), nv = dot(normal, view);
    if (nl <= 0.0 || nv <= 0.0) return vec3(0.0);
    vec3 halfVector = normalize(view + toLight);
    float nh = clamp(dot(normal, halfVector), 0.0, 1.0);
    float r = clamp(roughness, 0.04, 1.0);
    float alpha = r * r, a2 = alpha * alpha;
    float d = nh * nh * a2 + (1.0 - nh * nh);
    float distribution = a2 / (3.141592653589793 * d * d);
    float k = (r + 1.0) * (r + 1.0) / 8.0;
    float visibility = 0.25 / ((nl * (1.0 - k) + k) * (nv * (1.0 - k) + k));
    return schlickFresnel(specularF0(albedo, metalness), dot(view, halfVector)) * distribution * visibility;
}
// VDC $phong: provisional three-point curve and Blinn-Phong half-vector lobe.
float phongFresnel(float normalView, vec3 ranges) {
    float x = 1.0 - clamp(normalView, 0.0, 1.0);
    return x < 0.5 ? mix(ranges.x, ranges.y, x * 2.0) : mix(ranges.y, ranges.z, x * 2.0 - 1.0);
}
float phongTextureExponent(float red) { return clamp(red, 0.0, 1.0) * 149.0 + 1.0; }
vec3 phongBrdf(vec3 normal, vec3 view, vec3 toLight, float exponent, float boost,
               vec3 ranges, vec3 tint, float mask) {
    float nl = dot(normal, toLight), nv = dot(normal, view);
    if (nl <= 0.0 || nv <= 0.0) return vec3(0.0);
    float nh = clamp(dot(normal, normalize(view + toLight)), 0.0, 1.0);
    return max(tint, vec3(0.0)) * (pow(nh, max(exponent, 1.0)) * max(boost, 0.0) *
        max(phongFresnel(nv, ranges), 0.0) * clamp(mask, 0.0, 1.0));
}
vec3 specularIrradiance(vec3 measuredIrradiance, float normalLight, vec3 brdf) {
    if (normalLight <= 0.0) return vec3(0.0);
    return measuredIrradiance * brdf * (2.0 / (1.0 + normalLight));
}
// Environment maps and self-illumination (0.6.5 d), equally provisional. Karis,
// Physically Based Shading on Mobile (2014), analytic environment BRDF.
vec2 envBrdfScaleBias(float roughness, float normalView) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = clamp(roughness, 0.0, 1.0) * c0 + c1;
    float nv = clamp(normalView, 0.0, 1.0);
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}
vec3 envBrdf(vec3 f0, float roughness, float normalView) {
    vec2 ab = envBrdfScaleBias(roughness, normalView);
    return f0 * ab.x + ab.y;
}
float luminance(vec3 rgb) { return dot(rgb, vec3(0.2126, 0.7152, 0.0722)); }
float envmapLightScale(vec3 diffuseIrradiance, float scale) {
    return mix(1.0, luminance(diffuseIrradiance), scale);
}
vec3 envmapContrastSaturation(vec3 env, float contrast, float saturation) {
    vec3 contrasted = mix(env, env * env, contrast);
    return mix(vec3(luminance(contrasted)), contrasted, saturation);
}
vec3 envmapMask(bool hasMaskTexture, vec3 maskTexel, bool normalAlpha, float bumpAlpha,
                bool baseAlpha, float baseAlphaValue) {
    if (hasMaskTexture) return maskTexel;
    if (normalAlpha) return vec3(bumpAlpha);
    if (baseAlpha) return vec3(baseAlphaValue);
    return vec3(1.0);
}
float selfIllumMask(bool hasMaskTexture, float maskRed, float baseAlpha) {
    return hasMaskTexture ? maskRed : baseAlpha;
}
#endif
