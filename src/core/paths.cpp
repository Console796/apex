#include "core/paths.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <vector>

namespace apex {
namespace {

std::string_view filename(std::string_view normalized) {
    const auto slash = normalized.rfind('/');
    return slash == std::string_view::npos ? normalized : normalized.substr(slash + 1);
}

std::size_t extensionPosition(std::string_view name) {
    if (name == "." || name == "..") {
        return std::string_view::npos;
    }
    const auto dot = name.rfind('.');
    return dot == 0 ? std::string_view::npos : dot;
}

} // namespace

std::string normalizePath(std::string_view path) {
    std::string slashes(path);
    std::replace(slashes.begin(), slashes.end(), '\\', '/');
    const bool rooted = slashes.starts_with('/');
    std::vector<std::string_view> components;
    for (auto part : split(slashes, '/')) {
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!components.empty() && components.back() != "..") {
                components.pop_back();
            } else if (!rooted) {
                components.push_back(part);
            }
        } else {
            components.push_back(part);
        }
    }
    std::string result = rooted ? "/" : "";
    for (auto part : components) {
        if (!result.empty() && result.back() != '/') {
            result += '/';
        }
        result += part;
    }
    return result;
}

std::string lookupKey(std::string_view path) { return toLowerAscii(normalizePath(path)); }

std::string extension(std::string_view path) {
    const auto normalized = normalizePath(path);
    const auto name = filename(normalized);
    const auto dot = extensionPosition(name);
    return dot == std::string_view::npos ? std::string{} : std::string(name.substr(dot));
}

std::string stem(std::string_view path) {
    const auto normalized = normalizePath(path);
    const auto name = filename(normalized);
    return std::string(name.substr(0, extensionPosition(name)));
}

std::string join(std::string_view left, std::string_view right) {
    if (!right.empty() && (right.front() == '/' || right.front() == '\\')) {
        return normalizePath(right);
    }
    std::string combined(left);
    combined += '/';
    combined += right;
    // An empty left side must not turn a relative right side into an absolute path.
    return normalizePath(left.empty() ? right : std::string_view(combined));
}

} // namespace apex
