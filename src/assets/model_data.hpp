#pragma once

#include "core/math.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apex::assets {

struct ModelVertex {
    Vec3 position{};
    Vec3 normal{};
    Vec2 uv{};
};
struct ModelSubmesh {
    std::uint32_t firstIndex = 0, indexCount = 0;
    std::uint32_t bodyPart = 0, choice = 0, mesh = 0;
    std::int32_t material = 0; // Skin reference, remapped through the selected family.
};
struct ModelChoice {
    std::string name;
    AABB bounds;
};
struct ModelBodyPart {
    std::string name;
    std::uint32_t base = 1;
    std::vector<ModelChoice> choices;
};
struct ModelData {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<ModelSubmesh> submeshes;
    std::vector<ModelBodyPart> bodyParts;
    std::vector<std::vector<std::int16_t>> skins; // Family -> material index per skin ref.
    std::vector<std::string> materialNames;
    std::vector<std::string> materialDirectories;
    AABB bounds;
};
struct ModelDrawRange {
    std::uint32_t firstIndex = 0, indexCount = 0;
    std::int32_t material = 0;
};

// Empty/invalid body parts select zero; an invalid skin falls back to family zero.
std::vector<std::uint32_t> modelBodyChoices(const ModelData& model, std::uint32_t body);
std::vector<ModelDrawRange> modelDrawRanges(const ModelData& model, std::uint32_t body,
                                          std::uint32_t skin);

} // namespace apex::assets
