#include "assets/game_profile.hpp"

#include "core/json.hpp"

#include <fstream>
#include <iterator>
#include <sourcepp/Math.h>
#include <steampp/steampp.h>

namespace apex::assets {

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
    return json::write({json::Value::Object{
        {"addons", {json::Value::Object{{"enabled", {profile.addons.enabled}},
                                       {"list", {list}}, {"mounted", {mounted}}}}},
        {"name", {profile.name}}, {"game_directory", {profile.gameDirectory.generic_string()}},
        {"fgds", {fgds}}, {"tool_paths", {tools}},
        {"kv_escape_mode",
         {std::string(profile.escapeMode == kv1::EscapeMode::Strata ? "strata" : "legacy")}},
        {"vmf_write", {json::Value::Object{{"point_data", {profile.vmfWrite.pointData}},
                                          {"markers", {profile.vmfWrite.markers}}}}}}}) + '\n';
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

} // namespace apex::assets
