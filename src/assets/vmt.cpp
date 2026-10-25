#include "assets/vmt.hpp"

#include "core/paths.hpp"
#include "core/strings.hpp"

#include <set>

namespace apex::assets {
namespace {
std::string assetPath(std::string_view name, std::string_view suffix) {
    auto path = lookupKey(name);
    if (!path.starts_with("materials/")) {
        path = "materials/" + path;
    }
    if (!path.ends_with(suffix)) {
        path += suffix;
    }
    return path;
}
Result<std::string> read(const Vfs& vfs, std::string_view path) {
    auto blob = vfs.open(path);
    if (!blob) {
        return std::unexpected(blob.error());
    }
    return std::string(reinterpret_cast<const char*>(blob->data()), blob->size());
}
// Source patch insert only fills absent keys; replace only changes existing keys.
Result<void> merge(kv1::Node& target, const kv1::Node& changes, bool replace) {
    for (const auto& change : changes.children()) {
        auto existing = target.find(change.key());
        if (existing && existing->kind() == kv1::NodeKind::Block &&
            change.kind() == kv1::NodeKind::Block) {
            auto result = merge(*existing, change, replace);
            if (!result) {
                return result;
            }
        } else if (existing && replace) {
            *existing = change;
        } else if (!existing && !replace) {
            auto result = target.append(change);
            if (!result) {
                return result;
            }
        }
    }
    return {};
}
Result<kv1::Node> resolve(const MaterialReader& read, const std::string& path, kv1::EscapeMode mode,
                         std::set<std::string>& stack) {
    if (stack.size() >= 64 || !stack.insert(path).second) {
        return std::unexpected(Error{ErrorCode::ParseError, "VMT patch cycle/depth: " + path});
    }
    struct Pop {
        std::set<std::string>& stack;
        const std::string& path;
        ~Pop() { stack.erase(path); }
    } pop{stack, path};
    auto bytes = read(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto parsed = kv1::parse(std::move(*bytes), mode);
    if (!parsed) {
        return std::unexpected(parsed.error().withContext(path));
    }
    auto expanded = parsed->resolve([&](std::string_view include) { return read(include); });
    if (!expanded) {
        return std::unexpected(expanded.error().withContext(path));
    }
    auto children = expanded->root().children();
    if (children.size() != 1 || children.front().kind() != kv1::NodeKind::Block) {
        return std::unexpected(Error{ErrorCode::ParseError, "Expected one VMT shader: " + path});
    }
    const auto& root = children.front();
    if (!ciEquals(root.key(), "patch")) {
        return root;
    }
    auto include = root.find("include");
    if (!include) {
        return std::unexpected(Error{ErrorCode::ParseError, "Patch has no include: " + path});
    }
    auto included = include->decodedValue();
    if (!included) {
        return std::unexpected(included.error());
    }
    auto base = resolve(read, materialPath(*included), mode, stack);
    if (!base) {
        return base;
    }
    for (const auto& change : root.children()) {
        if (ciEquals(change.key(), "insert") || ciEquals(change.key(), "replace")) {
            if (change.kind() != kv1::NodeKind::Block) {
                return std::unexpected(Error{ErrorCode::ParseError, "Patch operation must be a block"});
            }
            auto result = merge(*base, change, ciEquals(change.key(), "replace"));
            if (!result) {
                return std::unexpected(result.error());
            }
        }
    }
    return base;
}
std::string value(const kv1::Node& root, std::string_view key) {
    auto node = root.find(key);
    return node ? node->decodedValue().value_or("") : "";
}
bool flag(const kv1::Node& root, std::string_view key) {
    auto node = root.find(key);
    return node && node->asDouble().value_or(0) != 0;
}
} // namespace

std::string materialPath(std::string_view name) { return assetPath(name, ".vmt"); }
std::string texturePath(std::string_view name) { return assetPath(name, ".vtf"); }
std::string_view Material::previewTexture() const {
    if (!toolViewportTexture.empty()) {
        return toolViewportTexture;
    }
    if (!toolTexture.empty()) {
        return toolTexture;
    }
    return baseTexture;
}
Result<Material> loadMaterial(const Vfs& vfs, std::string_view name, kv1::EscapeMode mode) {
    return parseMaterial([&](std::string_view path) { return read(vfs, path); }, name, mode);
}
Result<Material> parseMaterial(const MaterialReader& read, std::string_view name, kv1::EscapeMode mode) {
    std::set<std::string> stack;
    auto root = resolve(read, materialPath(name), mode, stack);
    if (!root) {
        return std::unexpected(root.error());
    }
    Material result;
    result.shader = root->key();
    result.baseTexture = value(*root, "$basetexture");
    result.baseTexture2 = value(*root, "$basetexture2");
    result.toolTexture = value(*root, "%tooltexture");
    result.toolViewportTexture = value(*root, "%ToolViewportTexture");
    result.keywords = value(*root, "%keywords") + " " + value(*root, "$keywords");
    result.hasToolTexture = root->find("%tooltexture") != nullptr;
    result.surfaceProp = value(*root, "$surfaceprop");
    result.translucent = flag(*root, "$translucent");
    result.alphaTest = flag(*root, "$alphatest");
    result.noCull = flag(*root, "$nocull");
    for (const auto& node : root->children()) {
        auto key = toLowerAscii(node.key());
        if (key.starts_with("%compile")) {
            result.compileFlags[key] = node.decodedValue().value_or("");
        }
    }
    return result;
}
} // namespace apex::assets
