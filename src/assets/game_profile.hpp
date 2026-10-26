#pragma once

#include "core/kv1.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apex::assets {

struct VmfWriteOptions {
    bool pointData = true;
    bool markers = true;
    bool operator==(const VmfWriteOptions&) const = default;
};

struct AddonSettings {
    enum class List { Hammer, Game, Apex };
    // The game's list holds both local and Workshop addons; Wine Hammer's can't see Workshop items.
    List list = List::Game;
    bool enabled = true;
    std::vector<std::filesystem::path> apexMounted;
    bool operator==(const AddonSettings&) const = default;
};

struct GameProfile {
    std::string name;
    // Directory containing gameinfo.txt, rather than the Steam install root.
    std::filesystem::path gameDirectory;
    std::vector<std::filesystem::path> fgds;
    std::map<std::string, std::filesystem::path> toolPaths;
    kv1::EscapeMode escapeMode = kv1::EscapeMode::Strata;
    VmfWriteOptions vmfWrite;
    AddonSettings addons;
    bool operator==(const GameProfile&) const = default;
};

Result<GameProfile> parseGameProfile(std::string_view json);
std::string writeGameProfile(const GameProfile& profile);
Result<GameProfile> loadGameProfile(const std::filesystem::path& file);
Result<void> saveGameProfile(const GameProfile& profile, const std::filesystem::path& file);
std::optional<GameProfile> detectP2ce();

} // namespace apex::assets
