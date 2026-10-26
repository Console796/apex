#pragma once

#include "assets/vfs.hpp"

#include <map>

namespace apex::assets {

struct Material {
    std::string shader;
    std::string baseTexture;
    std::string baseTexture2;
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
Result<Material> loadMaterial(const Vfs& vfs, std::string_view name,
                              kv1::EscapeMode mode = kv1::EscapeMode::Strata);
// The reader supplies owned text for canonical virtual paths; it may run on a worker.
using MaterialReader = std::function<Result<std::string>(std::string_view)>;
Result<Material> parseMaterial(const MaterialReader& read, std::string_view name,
                               kv1::EscapeMode mode = kv1::EscapeMode::Strata);
std::string materialPath(std::string_view name);
std::string texturePath(std::string_view name);

} // namespace apex::assets
