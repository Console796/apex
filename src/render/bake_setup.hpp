#pragma once

#include "assets/vfs.hpp"
#include "lighting/bake_scene.hpp"

namespace apex::render {
struct BakeSetup {
    lighting::BakeScene scene;
    std::size_t failedMaterials = 0, failedTextures = 0;
};
// Visible lightmapped brushes (world and brush entities; casters stay world/func_detail), linear $reflectivity (else base VTF) and compile flags.
BakeSetup setupBake(const document::Document& doc, const assets::Vfs* vfs);
} // namespace apex::render
