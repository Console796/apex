#pragma once

#include "assets/vmt.hpp"
#include "assets/asset_index.hpp"

#include <chrono>
#include <set>

namespace apex::assets {

struct MaterialIndexEntry {
    std::string name;
    std::string shader;
    std::vector<std::string> keywords;
    bool toolTexture = false;
    std::string previewTexture;
    DMat3 baseTextureTransform{1.0};
    std::optional<std::size_t> mount; // Vfs::provider: addon index, empty for game content
    std::string error;
};
struct MaterialIndexSnapshot {
    explicit MaterialIndexSnapshot(std::vector<MaterialIndexEntry> entries = {});
    std::vector<MaterialIndexEntry> entries; // sorted case-insensitively by name
    const MaterialIndexEntry* find(std::string_view name) const;
};
struct MaterialFilter {
    std::string name; // substring or full-name glob; keyword:word tokens match whole keywords
    std::string shader;
    bool toolTextures = true;
    // Unset: any source. Set: entries whose mount equals it (nullopt = game content, not an addon).
    std::optional<std::optional<std::size_t>> source;
    std::optional<std::set<std::string>> usedInMap; // material names, any spelling
};
std::vector<std::size_t> filterMaterials(const MaterialIndexSnapshot& snapshot,
                                       const MaterialFilter& filter);

struct MaterialKind {
    using Entry = MaterialIndexEntry;
    using Snapshot = MaterialIndexSnapshot;
    static std::function<std::vector<std::string>()> walk(const Vfs& vfs);
    static std::string key(std::string_view name);
};
using MaterialIndex = AssetIndex<MaterialKind>;
using MaterialIndexProgress = AssetIndexProgress;

} // namespace apex::assets
