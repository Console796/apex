#include "assets/unmounted_addons.hpp"

#include "assets/path_expand.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"

#include <set>
#include <vpkpp/format/VPK.h>

namespace apex::assets {
UnmountedAddonIndex::UnmountedAddonIndex(const AddonList& list, CancelToken token) {
    namespace fs = std::filesystem;
    for (std::size_t i = 0; i < list.entries.size(); ++i) {
        if (token.cancelled()) return;
        const auto& addon = list.entries[i];
        if (addon.mounted || addon.resolved.empty()) continue;
        const auto add = [&](std::string_view path) {
            const auto key = lookupKey(path);
            if (!key.starts_with(".assets/")) paths_.try_emplace(key, i);
        };
        std::error_code ec;
        for (fs::recursive_directory_iterator it(addon.resolved, ec), end; !ec && it != end; it.increment(ec)) {
            if (token.cancelled()) return;
            const auto relative = it->path().lexically_relative(addon.resolved);
            if (ciEquals(relative.begin()->string(), ".assets")) {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec)) add(relative.generic_string());
        }
        if (ec) problems_.push_back("Cannot index " + addon.dir + ": " + ec.message());
        std::set<fs::path> seen;
        for (const auto& name : addon.vpks) {
            auto files = detail::expand(addon.resolved / name);
            if (files.empty()) files.push_back(addon.resolved / name);
            for (auto file : files) {
                if (token.cancelled()) return;
                auto archive = vpkpp::VPK::open(file.string());
                if (!archive && !toLowerAscii(file.stem().string()).ends_with("_dir")) {
                    for (const auto& fallback : detail::expand(file.parent_path() / (file.stem().string() + "_dir.vpk"))) {
                        archive = vpkpp::VPK::open(fallback.string());
                        if (archive) break;
                    }
                }
                if (!archive) { problems_.push_back("Cannot index VPK " + file.string()); continue; }
                if (!seen.insert(fs::path(archive->getFilepath())).second) continue;
                archive->runForAllEntries([&](const std::string& path, const vpkpp::Entry&) { add(path); });
            }
        }
    }
}
std::optional<std::size_t> UnmountedAddonIndex::provider(std::string_view path) const {
    const auto found = paths_.find(lookupKey(path));
    return found == paths_.end() ? std::nullopt : std::optional(found->second);
}
} // namespace apex::assets
