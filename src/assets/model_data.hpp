#pragma once

#include "core/math.hpp"

#include <cstdint>
#include <functional>
#include <memory>
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
struct ModelSequence {
    std::string name;
    double fps = 30.0;
    int frames = 1;
    bool supported = false;
    std::string reason;
    AABB bounds; // Sequence bounds keep the inspector camera stable during playback.
};
struct ModelAnimationData;
struct ModelData {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<ModelSubmesh> submeshes;
    std::vector<ModelBodyPart> bodyParts;
    std::vector<std::vector<std::int16_t>> skins; // Family -> material index per skin ref.
    std::vector<std::string> materialNames;
    std::vector<std::string> materialDirectories;
    AABB bounds;
    std::vector<ModelSequence> sequences;
    std::shared_ptr<const ModelAnimationData> animation;
};
struct ModelDrawRange {
    std::uint32_t firstIndex = 0, indexCount = 0;
    std::int32_t material = 0;
};

// Empty/invalid body parts select zero; an invalid skin falls back to family zero.
std::vector<std::uint32_t> modelBodyChoices(const ModelData& model, std::uint32_t body);
std::vector<ModelDrawRange> modelDrawRanges(const ModelData& model, std::uint32_t body,
                                          std::uint32_t skin);

// The material name (as MaterialCache takes it, no "materials/" prefix) a draw range's material index
// resolves to: the first cdmaterials directory, in the model's order, for which `exists` finds the VMT.
// `exists` is given the canonical virtual path ("materials/...vmt"). Empty when the index is out of range
// or no directory holds the material.
std::string resolveModelMaterial(const ModelData& model, std::int32_t material,
                                 const std::function<bool(const std::string&)>& exists);

} // namespace apex::assets
