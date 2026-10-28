#pragma once

#include "assets/asset_index.hpp"

namespace apex::assets {

struct SoundIndexEntry {
    enum class Type { Raw, SoundScript };
    std::string name; // raw: sound/... path; script: event name
    Type type = Type::Raw;
    std::string script;
    std::vector<std::string> waves; // canonical sound/... paths, Source prefixes removed
    std::string channel, volume, pitch, soundlevel;
    std::optional<std::size_t> mount;
    std::string error;
};
struct SoundKind {
    using Entry = SoundIndexEntry;
    using Snapshot = AssetIndexSnapshot<SoundKind>;
    static std::function<std::vector<std::string>()> walk(const Vfs& vfs);
    static std::string key(std::string_view name);
};
using SoundIndex = AssetIndex<SoundKind>;
using SoundIndexSnapshot = SoundKind::Snapshot;
struct SoundFilter : AssetFilter { std::optional<SoundIndexEntry::Type> type; };
std::vector<std::size_t> filterSounds(const SoundIndexSnapshot& snapshot, const SoundFilter& filter);

} // namespace apex::assets
