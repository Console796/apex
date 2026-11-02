#pragma once

#include "lighting/bsp_lightmaps.hpp"
#include "render/lightmap_atlas.hpp"

namespace apex::render {

// A VRAD result in the form the Baked-style view draws: the compiled fragments packed with their own lattice.
struct VradLightmaps {
    LightmapAtlas atlas; // compiled = true
    std::vector<std::vector<Vec3>> radiance; // Per atlas face
    lighting::BspMapStats stats;
};

// Reads the BSP's HDR lighting and maps its faces onto `exported`, the instance-expanded copy of the
// document that was compiled (fragments are matched to its sides by position: P2CE writes no
// hammerfaceids). Worker-thread safe.
Result<VradLightmaps> loadVradLightmaps(const std::filesystem::path& bsp, const document::Document& exported,
                                        std::span<const document::InstanceSolidOrigin> origins);

} // namespace apex::render
