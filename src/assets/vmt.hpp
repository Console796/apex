#pragma once

#include "assets/vfs.hpp"
#include "core/math.hpp"

#include <map>

namespace apex::assets {

struct Material {
    std::string shader;
    // Resolved, lower-case $ keys; proxy contents are never material parameters.
    std::map<std::string, std::string> params;
    std::string_view bumpMap() const; // $bumpmap, else $normalmap (Refract, Water).
    bool ssbump() const;
    std::string_view envmap() const;
    DVec3 envmapTint() const;
    std::string_view envmapMask() const;
    bool normalMapAlphaEnvmapMask() const;
    bool baseAlphaEnvmapMask() const;
    double envmapLightScale() const; // Default 0.
    double envmapContrast() const;   // Default 0.
    double envmapSaturation() const; // Default 1.
    // $envmap env_cubemap (any case): the nearest env_cubemap probe, not a texture.
    bool envmapProbe() const;
    bool selfIllum() const;
    DVec3 selfIllumTint() const;
    std::string_view selfIllumMask() const;
    std::string_view mraoTexture() const;
    DVec3 mraoScale() const;
    bool model() const;
    DVec3 phongTint() const;
    bool baseMapAlphaPhongMask() const;
    bool phong() const;
    double phongExponent() const;
    double phongBoost() const;
    DVec3 phongFresnelRanges() const;
    std::string_view phongExponentTexture() const;
    bool localRefract() const;
    double localRefractDepth() const; // Default 0.
    double refractAmount() const;     // Default 0.
    DVec3 refractTint() const;        // Default white.
    std::string_view parameter(std::string_view key) const;
    double scalar(std::string_view key, double fallback = 0) const;
    DVec3 vector(std::string_view key, DVec3 fallback) const;
    std::string baseTexture;
    std::string baseTexture2;
    // Applied to normalized brush UVs after the VMF face mapping.
    DMat3 baseTextureTransform{1.0};
    std::string toolTexture;
    std::string toolViewportTexture;
    std::string surfaceProp;
    std::string keywords;
    bool hasToolTexture = false;
    bool translucent = false;
    bool alphaTest = false;
    bool noCull = false;
    // All %compile* values, with lower-case keys (including the '%' prefix).
    std::map<std::string, std::string> compileFlags;
    std::string_view previewTexture() const;
};

// Accepts a material name or materials/...vmt. Patch includes and KV directives use Vfs;
// includes are rooted virtual paths, not paths relative to the patch. Cycles are errors.
// VMT KeyValues use literal backslashes, independently of the VMF escape mode.
Result<Material> loadMaterial(const Vfs& vfs, std::string_view name);
// The reader supplies owned text for canonical virtual paths; it may run on a worker.
using MaterialReader = std::function<Result<std::string>(std::string_view)>;
Result<Material> parseMaterial(const MaterialReader& read, std::string_view name);
std::string materialPath(std::string_view name);
std::string texturePath(std::string_view name);

} // namespace apex::assets
