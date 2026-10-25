#include "assets/addons.hpp"

#include "assets/path_expand.hpp"
#include "core/kv3.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <iterator>

namespace apex::assets {
namespace fs = std::filesystem;
namespace {
Result<kv3::Document> read(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + file.string()});
    std::string bytes(std::istreambuf_iterator<char>{input}, {});
    if (input.bad()) return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + file.string()});
    auto doc = kv3::parse(bytes);
    if (!doc) return std::unexpected(doc.error().withContext(file.string()));
    return doc;
}
const std::string* string(const kv3::Value& value, std::string_view key) {
    auto child = value.find(key);
    return child ? child->get<std::string>() : nullptr;
}
}

fs::path resolveAddonPath(std::string_view dir) {
    std::string path(dir);
#ifdef _WIN32
    // Native separators and an upper-case drive, so results compare equal to paths built natively.
    std::ranges::replace(path, '/', '\\');
    if (path.size() >= 2 && path[1] == ':') path[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(path[0])));
#else
    std::ranges::replace(path, '\\', '/');
    if (path.size() >= 2 && path[1] == ':') {
        if (path[0] != 'z' && path[0] != 'Z') return {};
        path.erase(0, 2);
        if (path.empty() || path.front() != '/') path.insert(path.begin(), '/');
    }
#endif
    if (path.empty() || !fs::path(path).is_absolute()) return {};
    // Addon directories are literal paths, never glob patterns.
    if (path.find_first_of("*?") != std::string::npos) return {};
    for (const auto& candidate : detail::expand(path)) {
        std::error_code ec;
        if (fs::is_directory(candidate, ec)) return candidate;
    }
    return {};
}

Result<AddonList> readAddonList(const GameProfile& profile) try {
    AddonList result;
    result.source = profile.addons.list;
    if (!profile.addons.enabled) return result;
    auto gameDirectory = profile.gameDirectory.lexically_normal();
    if (gameDirectory.has_relative_path() && gameDirectory.filename().empty())
        gameDirectory = gameDirectory.parent_path();
    if (result.source == AddonSettings::List::Apex) {
        for (const auto& dir : profile.addons.apexMounted) {
            AddonEntry entry;
            entry.dir = dir.string();
            entry.mounted = true;
            entry.type = "local";
            result.entries.push_back(std::move(entry));
        }
    } else {
        auto files = result.source == AddonSettings::List::Hammer ?
            detail::expand(gameDirectory.parent_path() / "hammer/addons/addonlist.kv3") :
            std::vector<fs::path>{};
        if (files.empty()) {
            result.source = AddonSettings::List::Game;
            files = detail::expand(gameDirectory / "addons/addonlist.kv3");
        }
        if (files.empty()) return result;
        result.file = files.front();
        auto doc = read(result.file);
        if (!doc) return std::unexpected(doc.error());
        auto addons = doc->root.find("addons");
        auto array = addons ? addons->get<kv3::Value::Array>() : nullptr;
        auto invalid = [&] {
            return std::unexpected(Error{ErrorCode::ParseError, "Invalid addon list schema: " + result.file.string()});
        };
        if (!array) return invalid();
        for (const auto& item : *array) {
            AddonEntry entry;
            auto dir = string(item, "dir"), type = string(item, "type");
            auto mountedValue = item.find("mounted"), vpksValue = item.find("vpks");
            auto mounted = mountedValue ? mountedValue->get<bool>() : nullptr;
            auto vpks = vpksValue ? vpksValue->get<kv3::Value::Array>() : nullptr;
            if (!dir || !type || !mounted || !vpks) return invalid();
            entry.dir = *dir;
            entry.type = *type;
            entry.mounted = *mounted;
            for (const auto& vpk : *vpks) {
                auto name = vpk.get<std::string>();
                if (!name) return invalid();
                entry.vpks.push_back(*name);
            }
            result.entries.push_back(std::move(entry));
        }
    }
    for (auto& entry : result.entries) {
        entry.resolved = resolveAddonPath(entry.dir);
        std::string normalized = entry.dir;
        std::ranges::replace(normalized, '\\', '/');
        entry.name = fs::path(normalized).filename().string();
        if (entry.resolved.empty()) {
            result.problems.push_back("Cannot resolve addon directory: " + entry.dir);
            continue;
        }
        entry.name = entry.resolved.filename().string();
        if (result.source == AddonSettings::List::Apex) {
            for (const auto& vpk : detail::expand(entry.resolved / "*_dir.vpk")) {
                std::error_code ec;
                if (fs::is_regular_file(vpk, ec)) entry.vpks.push_back(vpk.filename().string());
            }
        }
        auto manifests = detail::expand(entry.resolved / "addon.kv3");
        auto manifest = read(manifests.empty() ? entry.resolved / "addon.kv3" : manifests.front());
        auto name = manifest ? string(manifest->root, "mod") : nullptr;
        if (name) entry.name = *name;
        else result.problems.push_back(manifest ? "Missing mod in " + entry.dir + "/addon.kv3" : manifest.error().what());
    }
    return result;
} catch (const fs::filesystem_error& error) {
    return std::unexpected(Error{ErrorCode::IoError, error.what()});
}

Result<void> setAddonMounted(const AddonList& list, std::size_t index, bool mounted) try {
    if ((list.source != AddonSettings::List::Hammer && list.source != AddonSettings::List::Game) ||
        list.file.empty() || index >= list.entries.size())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Invalid addon list or index"});
    std::ifstream input(list.file, std::ios::binary);
    if (!input) return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + list.file.string()});
    std::string bytes(std::istreambuf_iterator<char>{input}, {});
    if (input.bad()) return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + list.file.string()});
    input.close();
    auto doc = kv3::parse(bytes);
    if (!doc) return std::unexpected(doc.error().withContext(list.file.string()));
    auto addons = doc->root.find("addons");
    auto array = addons ? addons->get<kv3::Value::Array>() : nullptr;
    const auto* item = array && index < array->size() ? &(*array)[index] : nullptr;
    auto dir = item ? string(*item, "dir") : nullptr;
    auto value = item ? item->find("mounted") : nullptr;
    auto state = value ? value->get<bool>() : nullptr;
    if (!dir || *dir != list.entries[index].dir || !state)
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     "Addon list changed on disk; reload and try again"});
    if (*state == mounted) return {};
    const auto span = value->span();
    bytes.replace(span.offset, span.length, mounted ? "true" : "false");

    static std::atomic<unsigned> serial{0};
    auto temp = list.file;
    temp += ".apex-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(serial++) + ".tmp";
    // Exclusive creation prevents a collision from touching another writer's file.
#ifdef _WIN32
    auto* file = _wfopen(temp.c_str(), L"wbx");
#else
    auto* file = std::fopen(temp.c_str(), "wbx");
#endif
    if (!file) return std::unexpected(Error{ErrorCode::IoError, "Cannot create " + temp.string()});
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ec; fs::remove(path, ec); }
    } cleanup{temp};
    std::unique_ptr<std::FILE, decltype(&std::fclose)> output(file, &std::fclose);
    if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size() || std::fflush(file) != 0)
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + temp.string()});
    if (std::fclose(output.release()) != 0)
        return std::unexpected(Error{ErrorCode::IoError, "Cannot close " + temp.string()});
    fs::permissions(temp, fs::status(list.file).permissions());
    fs::rename(temp, list.file);
    return {};
} catch (const fs::filesystem_error& error) {
    return std::unexpected(Error{ErrorCode::IoError, error.what()});
}

} // namespace apex::assets
