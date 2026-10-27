#pragma once

#include "assets/asset_index.hpp"

#include <array>

namespace apex::assets {

struct ModelIndexEntry {
    std::string name; // canonical models/... .mdl path
    std::uint32_t flags = 0;
    bool staticProp = false;
    std::size_t bodyPartCount = 0;
    // mdlpp exposes skins[family][reference]; these are the two header dimensions.
    std::size_t skinsPerFamily = 0, skinFamilyCount = 0;
    std::array<float, 3> hullMin{}, hullMax{}, viewMin{}, viewMax{};
    std::optional<std::size_t> mount;
    std::string error;
};
struct ModelKind {
    using Entry = ModelIndexEntry;
    using Snapshot = AssetIndexSnapshot<ModelKind>;
    static std::function<std::vector<std::string>()> walk(const Vfs& vfs);
    static std::string key(std::string_view name);
};
using ModelIndex = AssetIndex<ModelKind>;
using ModelIndexSnapshot = ModelKind::Snapshot;
struct ModelFilter : AssetFilter { bool staticPropOnly = false; };
std::vector<std::size_t> filterModels(const ModelIndexSnapshot& snapshot, const ModelFilter& filter);

} // namespace apex::assets
