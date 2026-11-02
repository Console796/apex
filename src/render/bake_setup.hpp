#pragma once

#include "assets/vfs.hpp"
#include "lighting/bake_scene.hpp"

namespace apex::assets { struct Material; }

namespace apex::render {
// Shared linear VTF/$reflectivity rule for baked and realtime diffuse transport.
Vec3 bakeReflectivity(const assets::Material& material, Vec3 baseReflectivity);
struct BakeSetup {
    lighting::BakeScene scene;
    std::size_t failedMaterials = 0, failedTextures = 0;
};
// Visible lightmapped brushes (world and brush entities; casters stay world/func_detail), linear $reflectivity (else base VTF) and compile flags.
BakeSetup setupBake(const document::Document& doc, const assets::Vfs* vfs);
} // namespace apex::render
