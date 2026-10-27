#pragma once

#include "assets/asset_index.hpp"

namespace apex::assets {

struct ParticleIndexEntry {
    std::string name;
    std::string pcf;
    std::optional<std::size_t> mount;
    std::string error;
};
struct ParticleKind {
    using Entry = ParticleIndexEntry;
    using Snapshot = AssetIndexSnapshot<ParticleKind>;
    static std::function<std::vector<std::string>()> walk(const Vfs& vfs);
    static std::string key(std::string_view name);
};
using ParticleIndex = AssetIndex<ParticleKind>;
using ParticleIndexSnapshot = ParticleKind::Snapshot;
struct ParticleFilter : AssetFilter {};
std::vector<std::size_t> filterParticles(const ParticleIndexSnapshot& snapshot, const ParticleFilter& filter);

} // namespace apex::assets
