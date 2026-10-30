#pragma once

#include "core/math.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace apex::lighting {

// All material-response formulas are provisional until fitted to game captures (0.6 D2).
// Public reference: Brian Karis, Real Shading in Unreal Engine 4, SIGGRAPH 2013, eqs. 2-4, 8.
// https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf
inline DVec3 specularF0(DVec3 albedo, double metalness) {
    return glm::mix(DVec3(0.04), albedo, std::clamp(metalness, 0.0, 1.0));
}
inline DVec3 schlickFresnel(DVec3 f0, double viewHalf) {
    const double x = 1.0 - std::clamp(viewHalf, 0.0, 1.0);
    return f0 + (DVec3(1) - f0) * (x * x * x * x * x);
}
// Returns a BRDF, without the rendering equation's N.L or light intensity.
inline DVec3 ggxBrdf(DVec3 normal, DVec3 view, DVec3 toLight, DVec3 albedo,
                      double metalness, double roughness) {
    const double nl = glm::dot(normal, toLight), nv = glm::dot(normal, view);
    if (nl <= 0 || nv <= 0) return DVec3(0);
    const auto half = glm::normalize(view + toLight);
    const double nh = std::clamp(glm::dot(normal, half), 0.0, 1.0);
    const double r = std::clamp(roughness, 0.04, 1.0); // Finite delta-lobe approximation.
    const double alpha = r * r, a2 = alpha * alpha;
    const double d = nh * nh * a2 + (1.0 - nh * nh);
    const double distribution = a2 / (std::numbers::pi * d * d);
    const double k = (r + 1.0) * (r + 1.0) / 8.0;
    // Schlick-GGX G/(4 N.L N.V), cancelling the cosines analytically.
    const double visibility = 0.25 / ((nl * (1.0 - k) + k) * (nv * (1.0 - k) + k));
    return schlickFresnel(specularF0(albedo, metalness), glm::dot(view, half)) * distribution * visibility;
}
// Public parameter semantics: https://developer.valvesoftware.com/wiki/$phong
// Piecewise linear interpolation over 1-N.V, with knots at 0, 0.5 and 1.
inline double phongFresnel(double normalView, DVec3 ranges) {
    const double x = 1.0 - std::clamp(normalView, 0.0, 1.0);
    return x < 0.5 ? std::lerp(ranges.x, ranges.y, x * 2.0) :
                     std::lerp(ranges.y, ranges.z, x * 2.0 - 1.0);
}
inline double phongTextureExponent(double red) { return std::clamp(red, 0.0, 1.0) * 149.0 + 1.0; }
// Provisional, unnormalised Blinn-Phong (half-vector) lobe; boost is the artistic amplitude.
inline DVec3 phongBrdf(DVec3 normal, DVec3 view, DVec3 toLight, double exponent,
                        double boost, DVec3 ranges, DVec3 tint, double mask) {
    const double nl = glm::dot(normal, toLight), nv = glm::dot(normal, view);
    if (nl <= 0 || nv <= 0) return DVec3(0);
    const double nh = std::clamp(glm::dot(normal, glm::normalize(view + toLight)), 0.0, 1.0);
    return glm::max(tint, DVec3(0)) * (std::pow(nh, std::max(exponent, 1.0)) *
        std::max(boost, 0.0) * std::max(phongFresnel(nv, ranges), 0.0) * std::clamp(mask, 0.0, 1.0));
}
inline DVec3 specularIrradiance(DVec3 measuredIrradiance, double normalLight, DVec3 brdf) {
    if (normalLight <= 0) return DVec3(0);
    // Recover incoming radiance from measured irradiance, then apply BRDF*N.L:
    // N.L / surfaceResponse(N.L) = 2/(1+N.L). This keeps attenuation/cones identical.
    return measuredIrradiance * brdf * (2.0 / (1.0 + normalLight));
}

// Environment maps and self-illumination (plan 0.6.5 d), equally provisional.
// Karis, Physically Based Shading on Mobile (2014): analytic fit of the split-sum
// environment BRDF's scale (x) and bias (y) applied to F0, replacing a lookup texture.
inline DVec2 envBrdfScaleBias(double roughness, double normalView) {
    const glm::dvec4 c0(-1, -0.0275, -0.572, 0.022), c1(1, 0.0425, 1.04, -0.04);
    const glm::dvec4 r = std::clamp(roughness, 0.0, 1.0) * c0 + c1;
    const double nv = std::clamp(normalView, 0.0, 1.0);
    const double a004 = std::min(r.x * r.x, std::exp2(-9.28 * nv)) * r.x + r.y;
    return DVec2(-1.04, 1.04) * a004 + DVec2(r.z, r.w);
}
inline DVec3 envBrdf(DVec3 f0, double roughness, double normalView) {
    const auto ab = envBrdfScaleBias(roughness, normalView);
    return f0 * ab.x + ab.y;
}
// Rec. 709 luminance of linear RGB.
inline double luminance(DVec3 rgb) { return glm::dot(rgb, DVec3(0.2126, 0.7152, 0.0722)); }
// $envmaplightscale, inferred from its name: blends the reflection toward lighting-scaled.
inline double envmapLightScale(DVec3 diffuseIrradiance, double scale) {
    return std::lerp(1.0, luminance(diffuseIrradiance), scale);
}
// $envmapcontrast and $envmapsaturation, inferred from their names (provisional): contrast blends the
// reflection toward its square (1 = squared), saturation blends from its luminance (0 = grey) to itself.
inline DVec3 envmapContrastSaturation(DVec3 env, double contrast, double saturation) {
    const DVec3 contrasted = glm::mix(env, env * env, contrast);
    return glm::mix(DVec3(luminance(contrasted)), contrasted, saturation);
}
// Refract with $localrefract (Portal 2's light covers), provisional: the base texture is read where the view
// ray, bent by the tangent-space normal times $refractamount, meets a plane $localrefractdepth (UV units)
// behind the surface. tangentView points from the surface to the eye; its z is clamped to keep grazing
// views finite.
inline DVec2 localRefractUv(DVec2 uv, DVec3 tangentNormal, DVec3 tangentView, double amount, double depth) {
    const DVec2 parallax = DVec2(tangentView) / std::max(tangentView.z, 0.05);
    return uv + DVec2(tangentNormal) * amount - parallax * depth;
}
// Non-PBR envmap mask: $envmapmask RGB, else bump alpha with $normalmapalphaenvmapmask,
// else base alpha with $basealphaenvmapmask, else 1. Read directly, never inverted (unverified).
inline DVec3 envmapMask(bool hasMaskTexture, DVec3 maskTexel, bool normalAlpha, double bumpAlpha,
                        bool baseAlpha, double baseAlphaValue) {
    if (hasMaskTexture) return maskTexel;
    if (normalAlpha) return DVec3(bumpAlpha);
    if (baseAlpha) return DVec3(baseAlphaValue);
    return DVec3(1);
}
// $selfillum: $selfillummask R when present, else base alpha.
inline double selfIllumMask(bool hasMaskTexture, double maskRed, double baseAlpha) {
    return hasMaskTexture ? maskRed : baseAlpha;
}

} // namespace apex::lighting
