#pragma once

#include "assets/game_profile.hpp"
#include "assets/vfs.hpp"
#include "document/document.hpp"
#include "lighting/bake_scene.hpp"

#include <filesystem>

namespace apex::tools {

void expandBakeInstances(document::Document& doc, const std::filesystem::path& map,
                         const assets::GameProfile& profile);
struct BakeSetup {
    lighting::BakeScene scene;
    std::size_t failedMaterials = 0, failedTextures = 0;
};
// World + func_detail, visible brushes only; VTF linear reflectivity and compile flags.
// Does not mutate the document: the renderer and baker retain identical solid/side identities.
BakeSetup setupBake(const document::Document& doc, assets::Vfs* vfs);

} // namespace apex::tools
