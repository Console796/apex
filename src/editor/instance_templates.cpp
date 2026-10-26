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
}

std::optional<std::filesystem::path> resolveInstanceFile(const std::filesystem::path& map,
    std::string_view file, const std::optional<std::filesystem::path>& extraRoot) {
    if (file.empty()) return {};
    try {
        std::string spelling(file);
        std::ranges::replace(spelling, '\\', '/');
        std::vector<std::filesystem::path> roots{map.parent_path()};
        for (auto parent = map.parent_path(); !parent.empty();) {
            if (ciEquals(parent.filename().string(), "maps")) { roots.push_back(parent); break; }
            const auto next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
        if (extraRoot) roots.push_back(*extraRoot);
        for (const auto& root : roots)
            if (auto path = casePath(root / spelling)) return path;
    } catch (const std::exception&) { }
    return {};
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

} // namespace apex::editor
