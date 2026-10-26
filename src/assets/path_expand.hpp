#pragma once

#include "core/strings.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace apex::assets::detail {
namespace fs = std::filesystem;

inline bool matches(std::string_view pattern, std::string_view value) {
    std::size_t p = 0, v = 0, star = std::string_view::npos, retry = 0;
    while (v < value.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p;
            ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            retry = v;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            v = ++retry;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

// Resolve physical mount paths with the same ASCII case policy as virtual lookups. Expands
// wildcards component by component, so both custom/* and custom/*/*.vpk work on Linux.
inline std::vector<fs::path> expand(const fs::path& path) {
    std::vector<fs::path> paths{path.is_absolute() ? path.root_path() : fs::path{"."}};
    for (const auto& component : path.relative_path()) {
        if (component.empty()) continue;
        std::vector<fs::path> next;
        for (const auto& parent : paths) {
            if (component == "." || component == "..") {
                next.push_back(parent / component);
                continue;
            }
            std::error_code ec;
            for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
                if (matches(toLowerAscii(component.string()),
                            toLowerAscii(it->path().filename().string()))) {
                    next.push_back(it->path());
                }
            }
        }
        std::ranges::sort(next);
        paths = std::move(next);
        if (paths.empty()) {
            break;
        }
    }
    return paths;
}

} // namespace apex::assets::detail
