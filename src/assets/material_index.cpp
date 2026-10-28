#include "assets/material_index.hpp"

#include "assets/path_expand.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <sstream>

namespace apex::assets {
namespace {
std::string nameKey(std::string_view name) {
    auto path = materialPath(name);
    return path.substr(10, path.size() - 14);
}
}
MaterialIndexSnapshot::MaterialIndexSnapshot(std::vector<MaterialIndexEntry> values)
    : entries(std::move(values)) {
    std::ranges::sort(entries, [](const auto& a, const auto& b) { return ciLess(a.name, b.name); });
}
const MaterialIndexEntry* MaterialIndexSnapshot::find(std::string_view name) const {
    auto key = nameKey(name);
    auto it = std::lower_bound(entries.begin(), entries.end(), key,
        [](const auto& entry, const auto& value) { return ciLess(entry.name, value); });
    return it != entries.end() && ciEquals(it->name, key) ? &*it : nullptr;
}
std::vector<std::size_t> filterMaterials(const MaterialIndexSnapshot& snapshot, const MaterialFilter& filter) {
    std::vector<std::string> keywords;
    std::istringstream in(toLowerAscii(filter.name));
    std::string pattern;
    for (std::string word; in >> word;) {
        if (word.starts_with("keyword:")) keywords.push_back(word.substr(8));
        else { if (!pattern.empty()) pattern += ' '; pattern += word; }
    }
    std::set<std::string> used;
    if (filter.usedInMap)
        for (const auto& name : *filter.usedInMap) used.insert(nameKey(name));
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
        const auto& entry = snapshot.entries[i];
        const auto name = toLowerAscii(entry.name);
        if (!filter.toolTextures && entry.toolTexture) continue;
        if (!filter.shader.empty() && !ciEquals(filter.shader, entry.shader)) continue;
        if (filter.source && *filter.source != entry.mount) continue;
        if (filter.usedInMap && !used.contains(nameKey(entry.name))) continue;
        if (pattern.find_first_of("*?") != std::string::npos) {
            if (!detail::matches(pattern, name)) continue;
        } else if (name.find(pattern) == std::string::npos) continue;
        if (!std::ranges::all_of(keywords, [&](const auto& keyword) {
                return std::ranges::find(entry.keywords, keyword) != entry.keywords.end();
            })) continue;
        result.push_back(i);
    }
    return result;
}

std::function<std::vector<std::string>()> MaterialKind::walk(const Vfs& vfs) {
    return vfs.walkTask("materials", "*.vmt");
}
std::string MaterialKind::key(std::string_view name) { return nameKey(name); }
} // namespace apex::assets
