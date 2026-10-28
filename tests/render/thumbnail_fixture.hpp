#pragma once

#include "assets/material_cache.hpp"
#include "assets/model_data.hpp"

namespace apex::render::testing {
// Apex-authored asymmetric wedge, plus a distant alternate body choice that must stay hidden.
inline assets::ModelData thumbnailModel() {
    assets::ModelData model;
    const Vec3 corners[] = {{-12, -8, 0}, {12, -8, 0}, {12, 8, 0}, {-12, 8, 0},
                            {-12, -8, 18}, {12, -8, 6}, {12, 8, 6}, {-12, 8, 18}};
    const int faces[][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1},
                            {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
    for (const auto& face : faces) {
        const auto normal = glm::normalize(glm::cross(corners[face[1]] - corners[face[0]],
                                                       corners[face[2]] - corners[face[0]]));
        const auto first = static_cast<std::uint32_t>(model.vertices.size());
        const Vec2 uvs[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        int corner_index = 0;
        for (const auto corner : face) model.vertices.push_back({corners[corner], normal, uvs[corner_index++]});
        for (const auto index : {0u, 1u, 2u, 0u, 2u, 3u}) model.indices.push_back(first + index);
    }
    for (const auto& v : model.vertices) model.bounds.expand(DVec3(v.position));
    model.bodyParts.push_back({"body", 1, {{"wedge", model.bounds}, {"hidden", {}}}});
    model.submeshes.push_back({0, 36, 0, 0, 0, 0});
    const auto first = static_cast<std::uint32_t>(model.vertices.size());
    for (const auto p : {Vec3(1000, 0, 0), Vec3(1000, 10, 0), Vec3(1000, 0, 10)})
        model.vertices.push_back({p, {1, 0, 0}, {}});
    model.indices.insert(model.indices.end(), {first, first + 1, first + 2});
    model.submeshes.push_back({36, 3, 0, 1, 0, 0});
    model.skins = {{0}, {1}};
    model.materialNames = {"apex/thumbnail", "apex/alternate"};
    return model;
}
// An 8x8 two-colour checker in 2x2-texel cells, with a mip chain to 1x1 so mip choice is observable.
inline std::shared_ptr<const assets::CachedMaterial> thumbnailMaterial(
    std::array<std::uint8_t, 3> a = {230, 60, 40}, std::array<std::uint8_t, 3> b = {40, 90, 230},
    std::uint16_t size = 8) {
    auto material = std::make_shared<assets::CachedMaterial>();
    for (std::uint16_t s = size; s >= 1; s = static_cast<std::uint16_t>(s / 2)) {
        assets::TextureMip mip{s, s, 1, {}};
        for (unsigned y = 0; y < s; ++y)
            for (unsigned x = 0; x < s; ++x) {
                const auto& c = ((x / 2 + y / 2) & 1) ? b : a;
                mip.data.insert(mip.data.end(), {std::byte(c[0]), std::byte(c[1]), std::byte(c[2]), std::byte{255}});
            }
        material->texture.mips.push_back(std::move(mip));
        if (s == 1) break;
    }
    return material;
}
} // namespace apex::render::testing
