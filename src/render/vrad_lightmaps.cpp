#include "render/vrad_lightmaps.hpp"

namespace apex::render {

Result<VradLightmaps> loadVradLightmaps(const std::filesystem::path& bsp, const document::Document& exported,
                                        std::span<const document::InstanceSolidOrigin> origins) {
    auto fragments = lighting::readBspFragments(bsp);
    if (!fragments) return std::unexpected(fragments.error());
    auto mapped = lighting::mapBspFragments(*fragments, lighting::sideOwners(exported));
    VradLightmaps result;
    result.stats = mapped.stats;
    result.radiance = std::move(mapped.radiance);
    if (mapped.faces.empty())
        return std::unexpected(Error{ErrorCode::NotFound, "None of the compiled map's " +
            std::to_string(mapped.stats.fragments) + " faces matched a document side (" +
            std::to_string(mapped.stats.unowned) + " on no side, " + std::to_string(mapped.stats.noData) +
            " without light, " + std::to_string(mapped.stats.mismatched) + " unchartable)"});
    auto atlas = LightmapAtlas::pack(mapped.faces);
    if (!atlas) return std::unexpected(atlas.error());
    atlas->bindInstances(origins);
    atlas->compiled = true;
    result.atlas = std::move(*atlas);
    return result;
}

} // namespace apex::render
