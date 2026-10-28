#include "editor/asset_report.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <map>

namespace apex::editor {
namespace {
std::string value(const kv1::Node& block, std::string_view key) {
    const auto* entry = block.find(key);
    return entry ? entry->decodedValue().value_or("") : "";
}
template<class F> void walk(const document::Document& doc, const F& visit) {
    const auto recurse = [&](const auto& self, document::NodeId id) -> void {
        const auto h = *doc.handle(id);
        visit(h, *doc.get(h));
        for (auto child : doc.get(h)->children) self(self, child);
    };
    for (auto root : doc.roots()) recurse(recurse, root);
}
}
std::optional<AssetKind> entityAssetKind(const kv1::Node& entity, std::string_view key,
                                      const assets::EntityClassTable* classes) {
    if (ciEquals(key, "model") || ciEquals(key, "studio")) return AssetKind::Model;
    if (ciEquals(key, "sound")) return AssetKind::Sound;
    if (ciEquals(key, "effect_name")) return AssetKind::Particle;
    if (ciEquals(key, "file") && ciEquals(value(entity, "classname"), "func_instance")) return AssetKind::Instance;
    if (classes) if (const auto* cls = classes->find(value(entity, "classname"))) {
        for (const auto& property : cls->properties) if (ciEquals(property.name, key)) {
            const auto type = toLowerAscii(property.type);
            if (type == "material" || type == "texture") return AssetKind::Material;
            if (type == "studio" || type == "model") return AssetKind::Model;
            if (type == "sound" || type == "soundscript") return AssetKind::Sound;
            if (type == "particlesystem" || type == "particle") return AssetKind::Particle;
            return {};
        }
    }
    if (ciEquals(key, "material") || ciEquals(key, "texture")) return AssetKind::Material;
    return {};
}
AssetReport::AssetReport(const document::Document& doc, const AssetExists& exists,
                        const AssetProvider& unmounted, const AssetProvider& mounted,
                        const assets::EntityClassTable* classes) {
    std::map<std::pair<AssetKind, std::string>, MissingAsset> references;
    const auto add = [&](AssetKind kind, std::string path, AssetReference reference) {
        if (path.empty() || (kind == AssetKind::Model && path.starts_with('*'))) return;
        auto normalized = toLowerAscii(path);
        std::ranges::replace(normalized, '\\', '/');
        const auto key = std::pair{kind, normalized};
        auto [it, inserted] = references.try_emplace(key, MissingAsset{kind, path, normalized, {}, {}});
        (void)inserted;
        it->second.references.push_back(std::move(reference));
    };
    walk(doc, [&](const document::NodeHandle& h, const document::Node& node) {
        if (std::holds_alternative<Handle<document::Entity>>(h)) {
            for (const auto& entry : node.block->children()) if (entry.kind() == kv1::NodeKind::Value)
                if (auto kind = entityAssetKind(*node.block, entry.key(), classes))
                    add(*kind, entry.decodedValue().value_or(""), {node.id, 0, {}, std::string(entry.key())});
        } else if (std::holds_alternative<Handle<document::Solid>>(h)) {
            document::NodeId owner = node.parent;
            while (owner) {
                const auto parent = *doc.handle(owner);
                if (std::holds_alternative<Handle<document::Entity>>(parent)) break;
                owner = doc.get(parent)->parent;
            }
            for (const auto& side : node.block->children()) if (ciEquals(side.key(), "side")) {
                std::optional<document::SideId> sideId;
                if (const auto* id = side.find("id")) if (const auto parsed = id->asInt()) sideId = *parsed;
                add(AssetKind::Material, value(side, "material"), {owner, node.id, sideId, "material"});
            }
        }
    });
    for (auto& [key, entry] : references) {
        if (mounted) if (auto addon = mounted(key.first, key.second)) addons_.insert(*addon);
        if (!exists || !exists(key.first, key.second)) {
            if (unmounted) entry.addon = unmounted(key.first, key.second);
            missing_.push_back(std::move(entry));
        }
    }
}
Problems AssetReport::problems(const document::Document& doc, const ConnectionValidator& validator,
                              const std::function<bool(std::string_view)>& classKnown) const {
    Problems result{missing_, {}, {}};
    walk(doc, [&](const document::NodeHandle& h, const document::Node& node) {
        if (!std::holds_alternative<Handle<document::Entity>>(h)) return;
        auto classname = value(*node.block, "classname");
        if (classKnown && !classKnown(classname)) result.unknownClasses.push_back({node.id, classname});
        for (auto child : node.children) {
            const auto& block = *doc.get(*doc.handle(child))->block;
            if (!ciEquals(block.key(), "connections")) continue;
            for (std::size_t i = 0; i < block.children().size(); ++i) {
                const auto& entry = block.children()[i];
                if (entry.kind() != kv1::NodeKind::Value) continue;
                auto issues = validator.validate(node.id, document::parseConnection(entry.decodedValue().value_or("")), entry.key());
                if (!issues.empty()) result.invalidConnections.push_back({node.id, i, std::string(entry.key()), std::move(issues)});
            }
        }
    });
    return result;
}
} // namespace apex::editor
