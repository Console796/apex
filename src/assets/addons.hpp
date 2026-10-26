#pragma once

#include "assets/game_profile.hpp"

namespace apex::assets {

struct AddonEntry {
    std::string dir;
    std::filesystem::path resolved;
    bool mounted = false;
    std::string type;
    std::vector<std::string> vpks;
    std::string name;
};
struct AddonList {
    AddonSettings::List source = AddonSettings::List::Hammer;
    std::filesystem::path file;
    std::vector<AddonEntry> entries;
    std::vector<std::string> problems;
};

// Returns an existing directory, or an empty path. Does not modify the input or filesystem.
std::filesystem::path resolveAddonPath(std::string_view dir);
Result<AddonList> readAddonList(const GameProfile& profile);
Result<void> setAddonMounted(const AddonList& list, std::size_t index, bool mounted);

} // namespace apex::assets
