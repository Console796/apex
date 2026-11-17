#include "assets/game_profile.hpp"

#include "assets/path_expand.hpp"
#include "core/json.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sourcepp/Math.h>
#include <steampp/steampp.h>

namespace apex::assets {

std::filesystem::path GameProfile::instanceSearchRoot() const {
    std::error_code ec;
    const auto sdk = gameDirectory.parent_path() / "sdk_content" / "maps";
    return std::filesystem::is_directory(sdk, ec) ? sdk : gameDirectory / "maps";
}

std::filesystem::path GameProfile::toolDirectory() const {
    if (!customToolDirectory.empty()) return customToolDirectory;
#ifdef _WIN32
    return gameDirectory.parent_path() / "bin/win64";
#else
    return gameDirectory.parent_path() / "bin/linux64";
#endif
}

std::filesystem::path GameProfile::toolPath(std::string_view tool) const {
    if (auto it = toolPaths.find(std::string(tool)); it != toolPaths.end()) return it->second;
    if (!customToolDirectory.empty() && tool != "game") {
#ifdef _WIN32
        return customToolDirectory / (std::string(tool) + ".exe");
#else
        return customToolDirectory / std::string(tool);
#endif
    }
#ifdef _WIN32
    if (tool == "game") return toolDirectory() / "p2ce.exe";
    if (tool == "postcompiler") return toolDirectory() / "postcompiler/postcompiler.exe";
    return toolDirectory() / (std::string(tool) + ".exe");
#else
    if (tool == "game") return gameDirectory.parent_path() / "p2ce.sh";
    return toolDirectory() / std::string(tool);
#endif
}

Result<GameProfile> parseGameProfile(std::string_view source) {
    auto parsed = json::parse(source);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto* object = parsed->get<json::Value::Object>();
    auto invalid = [] {
        return std::unexpected(Error{ErrorCode::ParseError, "Invalid game profile schema"});
    };
    if (!object) {
        return invalid();
    }
    auto find = [&](std::string_view key) -> const json::Value* {
        auto it = object->find(key);
        return it == object->end() ? nullptr : &it->second;
    };
    auto string = [&](std::string_view key) -> const std::string* {
        auto v = find(key);
        return v ? v->get<std::string>() : nullptr;
    };
    const auto* name = string("name");
    const auto* directory = string("game_directory");
    if (!name || !directory || name->empty() || directory->empty()) {
        return invalid();
    }
    GameProfile result;
    result.name = *name;
    result.gameDirectory = *directory;
    if (auto v = find("fgds")) {
        auto array = v->get<json::Value::Array>();
        if (!array) {
            return invalid();
        }
        for (const auto& item : *array) {
            auto path = item.get<std::string>();
            if (!path) {
                return invalid();
            }
            result.fgds.emplace_back(*path);
        }
    }
    if (auto v = find("tool_paths")) {
        auto tools = v->get<json::Value::Object>();
        if (!tools) {
            return invalid();
        }
        for (const auto& [key, item] : *tools) {
            auto path = item.get<std::string>();
            if (!path) {
                return invalid();
            }
            result.toolPaths[key] = *path;
        }
    }
    if (auto v = find("tool_directory")) {
        auto path = v->get<std::string>();
        if (!path) {
            return invalid();
        }
        result.customToolDirectory = *path;
    }
    if (auto v = find("kv_escape_mode")) {
        auto mode = v->get<std::string>();
        if (!mode || (*mode != "strata" && *mode != "legacy")) {
            return invalid();
        }
        result.escapeMode = *mode == "strata" ? kv1::EscapeMode::Strata : kv1::EscapeMode::Legacy;
    }
    if (auto v = find("vmf_write")) {
        auto options = v->get<json::Value::Object>();
        if (!options) {
            return invalid();
        }
        for (auto [key, target] : {std::pair{"point_data", &result.vmfWrite.pointData},
                                   std::pair{"markers", &result.vmfWrite.markers}}) {
            if (auto it = options->find(key); it != options->end()) {
                auto flag = it->second.get<bool>();
                if (!flag) {
                    return invalid();
                }
                *target = *flag;
            }
        }
    }
    if (auto v = find("features")) {
        auto options = v->get<json::Value::Object>();
        if (!options) return invalid();
        for (auto [key, target] : {std::pair{"rt_light_modes", &result.features.rtLightModes},
                                   std::pair{"pbr_shader", &result.features.pbrShader}}) {
            if (auto it = options->find(key); it != options->end()) {
                auto flag = it->second.get<bool>();
                if (!flag) return invalid();
                *target = *flag;
            }
        }
    }
    if (auto v = find("addons")) {
        auto options = v->get<json::Value::Object>();
        if (!options) return invalid();
        if (auto it = options->find("enabled"); it != options->end()) {
            auto enabled = it->second.get<bool>();
            if (!enabled) return invalid();
            result.addons.enabled = *enabled;
        }
        if (auto it = options->find("list"); it != options->end()) {
            auto list = it->second.get<std::string>();
            if (!list) return invalid();
            if (*list == "hammer") result.addons.list = AddonSettings::List::Hammer;
            else if (*list == "game") result.addons.list = AddonSettings::List::Game;
            else if (*list == "apex") result.addons.list = AddonSettings::List::Apex;
            else return invalid();
        }
        if (auto it = options->find("mounted"); it != options->end()) {
            auto mounted = it->second.get<json::Value::Array>();
            if (!mounted) return invalid();
            for (const auto& item : *mounted) {
                auto path = item.get<std::string>();
                if (!path) return invalid();
                result.addons.apexMounted.emplace_back(*path);
            }
        }
    }
    return result;
}

std::string writeGameProfile(const GameProfile& profile) {
    json::Value::Array fgds;
    for (const auto& path : profile.fgds) {
        fgds.push_back({path.generic_string()});
    }
    json::Value::Object tools;
    for (const auto& [key, path] : profile.toolPaths) {
        tools[key] = {path.generic_string()};
    }
    json::Value::Array mounted;
    for (const auto& path : profile.addons.apexMounted) mounted.push_back({path.generic_string()});
    const std::string list = profile.addons.list == AddonSettings::List::Hammer ? "hammer" :
                            profile.addons.list == AddonSettings::List::Game ? "game" : "apex";
    json::Value::Object root{
        {"addons", {json::Value::Object{{"enabled", {profile.addons.enabled}},
                                       {"list", {list}}, {"mounted", {mounted}}}}},
        {"name", {profile.name}}, {"game_directory", {profile.gameDirectory.generic_string()}},
        {"fgds", {fgds}}, {"tool_paths", {tools}},
        {"kv_escape_mode",
         {std::string(profile.escapeMode == kv1::EscapeMode::Strata ? "strata" : "legacy")}},
        {"vmf_write", {json::Value::Object{{"point_data", {profile.vmfWrite.pointData}},
                                          {"markers", {profile.vmfWrite.markers}}}}}};
    // Written only when set, so profiles saved before the field existed stay byte-identical.
    if (!(profile.features == EngineFeatures{}))
        root["features"] = {json::Value::Object{{"rt_light_modes", {profile.features.rtLightModes}},
                                                {"pbr_shader", {profile.features.pbrShader}}}};
    if (!profile.customToolDirectory.empty()) root["tool_directory"] = {profile.customToolDirectory.generic_string()};
    return json::write({std::move(root)}) + '\n';
}

Result<GameProfile> loadGameProfile(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + file.string()});
    }
    std::string bytes(std::istreambuf_iterator<char>{input}, {});
    if (input.bad()) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + file.string()});
    }
    return parseGameProfile(bytes);
}

Result<void> saveGameProfile(const GameProfile& profile, const std::filesystem::path& file) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << writeGameProfile(profile);
    output.close();
    if (!output) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + file.string()});
    }
    return {};
}

std::optional<GameProfile> detectP2ce() {
    steampp::Steam steam;
    const auto install = steam.getAppInstallDir(440000);
    if (install.empty() || !std::filesystem::is_regular_file(install / "p2ce/gameinfo.txt")) {
        return {};
    }
    GameProfile result;
    result.name = "Portal 2: Community Edition";
    result.gameDirectory = install / "p2ce";
    result.fgds = {result.gameDirectory / "p2ce.fgd"};
#ifdef _WIN32
    const auto bin = install / "bin/win64";
    constexpr auto suffix = ".exe";
#else
    const auto bin = install / "bin/linux64";
    constexpr auto suffix = "";
#endif
    for (auto tool : {"vbsp", "vvis", "vrad"}) {
        result.toolPaths[tool] = bin / (std::string(tool) + suffix);
    }
    return result;
}

namespace {

std::filesystem::path firstExisting(const std::filesystem::path& directory, std::initializer_list<std::string_view> names) {
    for (auto name : names) {
        auto found = detail::expand(directory / std::string(name));
        for (const auto& path : found) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec)) return path;
        }
    }
    return {};
}

std::vector<std::filesystem::path> fgdsIn(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> result;
    for (const auto& path : detail::expand(directory / "*.fgd")) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) result.push_back(path);
    }
    std::ranges::sort(result);
    return result;
}

} // namespace

Result<GameProfile> suggestGameProfile(std::filesystem::path gameDirectory) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (ciEquals(gameDirectory.filename().string(), "gameinfo.txt") && !fs::is_directory(gameDirectory, ec))
        gameDirectory = gameDirectory.parent_path();
    gameDirectory = fs::absolute(gameDirectory, ec).lexically_normal();
    if (gameDirectory.has_relative_path() && gameDirectory.filename().empty()) gameDirectory = gameDirectory.parent_path();
    const auto gameinfos = detail::expand(gameDirectory / "gameinfo.txt");
    if (gameinfos.empty()) {
        return std::unexpected(Error{ErrorCode::NotFound, "No gameinfo.txt in " + gameDirectory.string()});
    }
    std::ifstream input(gameinfos.front(), std::ios::binary);
    std::string bytes(std::istreambuf_iterator<char>{input}, {});
    if (!input && !input.eof()) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + gameinfos.front().string()});
    }
    const bool strataMounts = !detail::expand(gameDirectory / "cfg/mounts.kv").empty();
    GameProfile result;
    result.gameDirectory = gameDirectory;
    // Legacy parsing is the safe read for an unknown game: Strata escapes are a superset only on write.
    auto parsed = kv1::parse(std::move(bytes), kv1::EscapeMode::Legacy);
    const kv1::Node* gameinfo = parsed ? parsed->root().find("GameInfo") : nullptr;
    if (!gameinfo || !gameinfo->find("FileSystem") || !gameinfo->find("FileSystem")->find("SearchPaths")) {
        return std::unexpected(Error{ErrorCode::ParseError, "gameinfo.txt has no GameInfo/FileSystem/SearchPaths block"});
    }
    if (const auto* game = gameinfo->find("game"); game && game->kind() == kv1::NodeKind::Value)
        result.name = game->decodedValue().value_or("");
    if (result.name.empty()) result.name = gameDirectory.filename().string();

    const auto install = gameDirectory.parent_path();
    // Strata ships 64-bit tools under bin/<platform>64; Valve's Source games keep them in bin/.
#ifdef _WIN32
    const std::initializer_list<std::string_view> toolDirs{"bin/win64", "bin/x64", "bin"};
    constexpr std::string_view suffix = ".exe";
#else
    const std::initializer_list<std::string_view> toolDirs{"bin/linux64", "bin"};
    constexpr std::string_view suffix = "";
#endif
    for (auto dir : toolDirs) {
        const auto vbsp = firstExisting(install / std::string(dir), {std::string("vbsp") + std::string(suffix)});
        if (vbsp.empty()) continue;
        const auto directory = vbsp.parent_path();
        if (directory != result.toolDirectory()) result.customToolDirectory = directory;
        for (auto tool : {"vbsp", "vvis", "vrad"}) {
            const auto path = firstExisting(directory, {std::string(tool) + std::string(suffix)});
            if (!path.empty()) result.toolPaths[tool] = path;
        }
        break;
    }
    const auto modName = gameDirectory.filename().string();
#ifdef _WIN32
    const auto game = firstExisting(install, {modName + ".exe", "hl2.exe", "p2ce.exe", "portal2.exe", "left4dead2.exe",
                                              "csgo.exe", "swarm.exe"});
#else
    const auto game = firstExisting(install, {modName + ".sh", "hl2.sh", "p2ce.sh", modName + "_linux", "hl2_linux",
                                              "portal2.sh"});
#endif
    if (!game.empty()) result.toolPaths["game"] = game;

    result.fgds = fgdsIn(gameDirectory);
    if (result.fgds.empty()) {
        for (auto dir : toolDirs) {
            result.fgds = fgdsIn(install / std::string(dir));
            if (!result.fgds.empty()) break;
        }
    }
    // Strata is recognised by its mount file or its 64-bit tool layout; anything else is treated as
    // Valve Source, whose Hammer reads KeyValues without escape sequences.
    const bool strata = strataMounts || (result.toolPaths.contains("vbsp") && result.customToolDirectory.empty());
    result.escapeMode = strata ? kv1::EscapeMode::Strata : kv1::EscapeMode::Legacy;
    result.vmfWrite = {strata, strata};
    result.features = {strata, strata};
    return result;
}

std::string gameProfileFileName(std::string_view name) {
    std::string result;
    for (unsigned char c : name) {
        const bool safe = std::isalnum(c) || c == '-' || c == '_' || c == ' ' || c == '.' || c >= 0x80;
        result += safe ? static_cast<char>(c) : '_';
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) result.pop_back();
    while (!result.empty() && (result.front() == ' ' || result.front() == '.')) result.erase(result.begin());
    if (result.empty()) result = "game";
    return result + ".json";
}

} // namespace apex::assets
