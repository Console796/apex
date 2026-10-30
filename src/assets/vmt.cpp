#include "assets/vmt.hpp"

#include "core/paths.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <cctype>
#include <locale>
#include <set>
#include <sstream>

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
// Choose the desktop DX9+/HDR branch consistently for every parameter, including base texture.
void parameters(const kv1::Node& root, std::map<std::string, std::string>& params) {
    for (const auto& node : root.children()) {
        const auto key = toLowerAscii(node.key());
        if (node.kind() != kv1::NodeKind::Block && key.starts_with("$"))
            params[key] = node.decodedValue().value_or("");
    }
    for (const auto& node : root.children()) {
        const auto key = toLowerAscii(node.key());
        if (node.kind() == kv1::NodeKind::Block &&
            (key == ">=dx90" || key == ">dx90" || key == ">=dx95" || key == "hdr"))
            parameters(node, params);
    }
}
DMat3 textureTransform(std::string text) {
    DVec2 center{0.5}, scale{1.0}, translation{0.0};
    double angle = 0.0;
    std::istringstream in(toLowerAscii(text));
    in.imbue(std::locale::classic());
    for (std::string key; in >> key;) {
        if (key == "center") in >> center.x >> center.y;
        else if (key == "scale") in >> scale.x >> scale.y;
        else if (key == "rotate") in >> angle;
        else if (key == "translate") in >> translation.x >> translation.y;
        else return DMat3{1.0};
        if (!in || !std::isfinite(center.x) || !std::isfinite(center.y) ||
            !std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(angle) ||
            !std::isfinite(translation.x) || !std::isfinite(translation.y)) return DMat3{1.0};
    }
    // Source: translate to the pivot, scale, rotate, then restore the pivot and translate.
    const double c = std::cos(glm::radians(angle)), s = std::sin(glm::radians(angle));
    DMat3 result{{c * scale.x, s * scale.x, 0}, {-s * scale.y, c * scale.y, 0}, {0, 0, 1}};
    result[2] = DVec3{center + translation - DVec2(result * DVec3{center, 0}), 1};
    return result;
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
std::string_view Material::parameter(std::string_view key) const {
    const auto found = params.find(toLowerAscii(key));
    return found == params.end() ? std::string_view{} : found->second;
}
double Material::scalar(std::string_view key, double fallback) const {
    std::istringstream in{std::string(parameter(key))};
    in.imbue(std::locale::classic());
    double result;
    if (!(in >> result) || !std::isfinite(result)) return fallback;
    in >> std::ws;
    return in.eof() ? result : fallback;
}
DVec3 Material::vector(std::string_view key, DVec3 fallback) const {
    auto text = std::string(parameter(key));
    const bool bytes = text.starts_with("{");
    for (auto& c : text) if (c == '[' || c == ']' || c == '{' || c == '}') c = ' ';
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    DVec3 result;
    if (!(in >> result.x) || !std::isfinite(result.x)) return fallback;
    in >> std::ws;
    if (in.eof()) result = DVec3(result.x); // VMT colours also accept a uniform scalar.
    else if (!(in >> result.y >> result.z) || !std::isfinite(result.y) || !std::isfinite(result.z)) return fallback;
    return bytes ? result / 255.0 : result;
}
std::string_view Material::bumpMap() const { return parameter("$bumpmap"); }
bool Material::ssbump() const { return scalar("$ssbump") != 0; }
std::string_view Material::envmap() const { return parameter("$envmap"); }
DVec3 Material::envmapTint() const { return vector("$envmaptint", DVec3(1)); }
std::string_view Material::envmapMask() const { return parameter("$envmapmask"); }
bool Material::normalMapAlphaEnvmapMask() const { return scalar("$normalmapalphaenvmapmask") != 0; }
bool Material::baseAlphaEnvmapMask() const { return scalar("$basealphaenvmapmask") != 0; }
double Material::envmapLightScale() const { return scalar("$envmaplightscale"); }
double Material::envmapContrast() const { return scalar("$envmapcontrast"); }
double Material::envmapSaturation() const { return scalar("$envmapsaturation", 1); }
bool Material::envmapProbe() const {
    const auto name = envmap();
    return std::ranges::equal(name, std::string_view("env_cubemap"),
                              [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}
bool Material::selfIllum() const { return scalar("$selfillum") != 0; }
std::string_view Material::selfIllumMask() const { return parameter("$selfillummask"); }
DVec3 Material::selfIllumTint() const { return vector("$selfillumtint", DVec3(1)); }
std::string_view Material::mraoTexture() const { return parameter("$mraotexture"); }
DVec3 Material::mraoScale() const { return vector("$mraoscale", DVec3(1)); }
bool Material::model() const { return scalar("$model") != 0; }
DVec3 Material::phongTint() const { return vector("$phongtint", DVec3(1)); }
bool Material::baseMapAlphaPhongMask() const { return scalar("$basemapalphaphongmask") != 0; }
bool Material::phong() const { return scalar("$phong") != 0; }
double Material::phongExponent() const { return scalar("$phongexponent", 5); }
double Material::phongBoost() const { return scalar("$phongboost", 1); }
DVec3 Material::phongFresnelRanges() const { return vector("$phongfresnelranges", {0, 0.5, 1}); }
std::string_view Material::phongExponentTexture() const { return parameter("$phongexponenttexture"); }
Result<Material> loadMaterial(const Vfs& vfs, std::string_view name) {
    return parseMaterial([&](std::string_view path) { return read(vfs, path); }, name);
}
Result<Material> parseMaterial(const MaterialReader& read, std::string_view name) {
    std::set<std::string> stack;
    auto root = resolve(read, materialPath(name), kv1::EscapeMode::Legacy, stack);
    if (!root) {
        return std::unexpected(root.error());
    }
    Material result;
    result.shader = root->key();
    parameters(*root, result.params);
    result.baseTexture = result.parameter("$basetexture");
    result.baseTexture2 = result.parameter("$basetexture2");
    result.baseTextureTransform = textureTransform(std::string(result.parameter("$basetexturetransform")));
    result.toolTexture = value(*root, "%tooltexture");
    result.toolViewportTexture = value(*root, "%ToolViewportTexture");
    result.keywords = value(*root, "%keywords") + " " + std::string(result.parameter("$keywords"));
    result.hasToolTexture = root->find("%tooltexture") != nullptr;
    result.surfaceProp = result.parameter("$surfaceprop");
    result.translucent = result.scalar("$translucent") != 0;
    result.alphaTest = result.scalar("$alphatest") != 0;
    result.noCull = result.scalar("$nocull") != 0;
    for (const auto& node : root->children()) {
        auto key = toLowerAscii(node.key());
        if (key.starts_with("%compile")) {
            result.compileFlags[key] = node.decodedValue().value_or("");
        }
    }
    return result;
}
} // namespace apex::assets
