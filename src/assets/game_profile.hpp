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

// What the game's engine branch implements, so the preview doesn't draw features it would not show.
// Defaults are Strata Source's (and P2CE's); a profile for any other game turns them off unless set.
struct EngineFeatures {
    // light_rt's _specularmode/_directmode/_indirectmode keys. Off: lights are plain VRAD lights.
    bool rtLightModes = true;
    // The `pbr` material shader. Off: such materials are not shaded as PBR.
    bool pbrShader = true;
    bool operator==(const EngineFeatures&) const = default;
};

struct GameProfile {
    std::string name;
    // Directory containing gameinfo.txt, rather than the Steam install root.
    std::filesystem::path gameDirectory;
    std::vector<std::filesystem::path> fgds;
    std::map<std::string, std::filesystem::path> toolPaths;
    kv1::EscapeMode escapeMode = kv1::EscapeMode::Strata;
    VmfWriteOptions vmfWrite;
    EngineFeatures features;
    AddonSettings addons;
    // Where the compile tools and their libraries live; empty means the Strata layout (bin/linux64, bin/win64).
    std::filesystem::path customToolDirectory;
    // Last place instance files are looked up (D7 root 3): the SDK's source maps beside the install when
    // present, since the game's own maps folder holds compiled maps rather than VMFs.
    std::filesystem::path instanceSearchRoot() const;
    std::filesystem::path toolDirectory() const;
    std::filesystem::path toolPath(std::string_view name) const;
    bool operator==(const GameProfile&) const = default;
};

Result<GameProfile> parseGameProfile(std::string_view json);
std::string writeGameProfile(const GameProfile& profile);
Result<GameProfile> loadGameProfile(const std::filesystem::path& file);
Result<void> saveGameProfile(const GameProfile& profile, const std::filesystem::path& file);
std::optional<GameProfile> detectP2ce();
// A starting profile for any Source game, from the folder holding its gameinfo.txt (a path to gameinfo.txt
// itself is accepted). The name comes from gameinfo's `game` key; FGDs, compile tools, the game executable
// and the escape mode are guesses from the install layout, for the Add Game dialog to show and correct.
Result<GameProfile> suggestGameProfile(std::filesystem::path gameDirectory);
// A JSON file name for a user profile: the name with characters unsafe in file names replaced by '_'.
std::string gameProfileFileName(std::string_view name);

} // namespace apex::assets
