#include "editor/instance_templates.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <fstream>

namespace apex::editor {
namespace {
std::optional<std::filesystem::path> casePath(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    auto current = path.root_path();
    std::error_code ec;
    for (const auto& part : path.relative_path()) {
        if (part == "." || part == "..") { current /= part; continue; }
        const auto exact = current / part;
        if (fs::exists(exact, ec)) { current = exact; continue; }
        std::vector<fs::path> matches;
        for (fs::directory_iterator it(current.empty() ? fs::path(".") : current, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (ciEquals(it->path().filename().string(), part.string())) matches.push_back(it->path());
        }
        if (ec || matches.empty()) return {};
        std::ranges::sort(matches);
        current = matches.front();
    }
    if (!fs::is_regular_file(current, ec)) return {};
    auto canonical = fs::canonical(current, ec);
    return ec ? std::nullopt : std::optional(canonical);
}
bool contained(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto p = path.begin(), r = root.begin();
    for (; r != root.end(); ++r, ++p) if (p == path.end() || *p != *r) return false;
    return true;
}
}

namespace {
struct InstanceRoots {
    std::string spelling;
    std::vector<std::filesystem::path> roots;
    std::vector<std::filesystem::path> allowed;
};
std::optional<InstanceRoots> instanceRoots(const std::filesystem::path& map, std::string_view file,
                                           const std::optional<std::filesystem::path>& extraRoot) {
    if (file.empty()) return {};
    InstanceRoots result;
    result.spelling = std::string(file);
    std::ranges::replace(result.spelling, '\\', '/');
    const auto& spelling = result.spelling;
    if (spelling.starts_with('/') || (spelling.size() >= 2 &&
        ((spelling[0] >= 'A' && spelling[0] <= 'Z') || (spelling[0] >= 'a' && spelling[0] <= 'z')) && spelling[1] == ':'))
        return {};
    std::optional<std::filesystem::path> mapsRoot;
    for (auto parent = map.parent_path(); !parent.empty();) {
        if (ciEquals(parent.filename().string(), "maps")) { mapsRoot = parent; break; }
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    result.roots.push_back(map.parent_path());
    if (mapsRoot && *mapsRoot != map.parent_path()) result.roots.push_back(*mapsRoot);
    if (extraRoot) result.roots.push_back(*extraRoot);
    const auto allowedMapRoot = mapsRoot.value_or(map.parent_path());
    {
        std::error_code ec;
        auto canonical = std::filesystem::canonical(allowedMapRoot, ec);
        if (!ec) result.allowed.push_back(std::move(canonical));
    }
    if (extraRoot) {
        std::error_code ec;
        auto canonical = std::filesystem::canonical(*extraRoot, ec);
        if (!ec) result.allowed.push_back(std::move(canonical));
    }
    return result;
}
}

std::optional<std::filesystem::path> resolveInstanceFile(const std::filesystem::path& map,
    std::string_view file, const std::optional<std::filesystem::path>& extraRoot) {
    try {
        const auto roots = instanceRoots(map, file, extraRoot);
        if (!roots) return {};
        for (const auto& root : roots->roots) {
            if (auto path = casePath(root / roots->spelling); path && std::ranges::any_of(roots->allowed,
                [&](const auto& allowedRoot) { return contained(*path, allowedRoot); })) return path;
        }
    } catch (const std::exception&) { }
    return {};
}

std::vector<std::filesystem::path> instanceFileCandidates(const std::filesystem::path& map,
    std::string_view file, const std::optional<std::filesystem::path>& extraRoot) {
    std::vector<std::filesystem::path> result;
    try {
        if (const auto roots = instanceRoots(map, file, extraRoot))
            for (const auto& root : roots->roots) result.push_back((root / roots->spelling).lexically_normal());
    } catch (const std::exception&) { }
    return result;
}

std::vector<std::string> proxyNames(const document::Document& doc) {
    std::map<std::string, std::string> names;
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto h = *doc.handle(id);
        const auto* node = doc.get(h);
        if (std::holds_alternative<Handle<document::Entity>>(h)) {
            const auto* cls = node->block->find("classname");
            const auto* name = node->block->find("targetname");
            if (cls && name && ciEquals(cls->decodedValue().value_or(""), "func_instance_io_proxy")) {
                auto value = name->decodedValue().value_or("");
                if (!value.empty()) names.try_emplace(toLowerAscii(value), value);
            }
        }
        for (auto child : node->children) self(self, child);
    };
    for (auto root : doc.roots()) walk(walk, root);
    std::vector<std::string> result;
    for (const auto& [key, spelling] : names) { (void)key; result.push_back(spelling); }
    return result;
}

bool looksLikeInstanceFile(const document::Document& doc) {
    bool found = false;
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto h = *doc.handle(id);
        const auto* node = doc.get(h);
        if (std::holds_alternative<Handle<document::Entity>>(h)) {
            const auto* cls = node->block->find("classname");
            found |= cls && ciEquals(cls->decodedValue().value_or(""), "func_instance_io_proxy");
        }
        for (auto child : node->children) self(self, child);
    };
    for (auto root : doc.roots()) walk(walk, root);
    return found;
}

Result<InstanceTemplates::Template> InstanceTemplates::load(const std::filesystem::path& resolved,
    std::span<const std::filesystem::path> ancestors) {
    try {
        std::error_code ec;
        auto key = std::filesystem::weakly_canonical(resolved, ec);
        if (ec) key = resolved.lexically_normal();
        if (ancestors.size() >= depthLimit)
            return std::unexpected(Error{ErrorCode::InvalidArgument, "instance depth limit reached"});
        for (const auto& ancestor : ancestors) {
            auto path = std::filesystem::weakly_canonical(ancestor, ec);
            if (!ec && path == key)
                return std::unexpected(Error{ErrorCode::InvalidArgument, "instance cycle"});
        }
        if (const auto it = cache_.find(key); it != cache_.end()) return it->second;
        auto result = [&]() -> Result<Template> {
            try {
                std::ifstream stream(key, std::ios::binary);
                if (!stream) return std::unexpected(Error{ErrorCode::IoError, "cannot open instance file"});
                const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
                if (stream.bad()) return std::unexpected(Error{ErrorCode::IoError, "cannot read instance file"});
                auto file = vmf::parse(bytes);
                if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
                if (!file) return std::unexpected(file.error());
                return std::make_shared<const document::Document>(*file, 0);
            } catch (const std::exception& error) {
                return std::unexpected(Error{ErrorCode::IoError, error.what()});
            }
        }();
        cache_.emplace(key, result);
        return result;
    } catch (const std::exception& error) {
        return std::unexpected(Error{ErrorCode::IoError, error.what()});
    }
}

Result<InstanceTemplates::Template> InstanceTemplates::invalidate(const std::filesystem::path& resolved) {
    std::error_code ec;
    auto key = std::filesystem::weakly_canonical(resolved, ec);
    if (ec) key = resolved.lexically_normal();
    const auto found = cache_.find(key);
    const auto previous = found == cache_.end() ? std::optional<Template>{} :
        found->second ? std::optional<Template>{*found->second} : std::optional<Template>{};
    cache_.erase(key);
    auto result = load(key);
    if (!result && previous) cache_.insert_or_assign(key, *previous);
    return result;
}

} // namespace apex::editor
