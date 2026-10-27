#include "editor/find_replace.hpp"

#include "core/strings.hpp"
#include "document/connection_codec.hpp"
#include "editor/selection_expand.hpp"

#include <algorithm>
#include <map>

namespace apex::editor {
namespace {
Error invalid(std::string text) { return {ErrorCode::InvalidArgument, std::move(text)}; }
bool word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
std::string replaceText(std::string_view original, const TextReplaceOptions& options) {
    if (options.find.empty()) return std::string(original);
    const auto text = options.caseSensitive ? std::string(original) : toLowerAscii(original);
    const auto pattern = options.caseSensitive ? options.find : toLowerAscii(options.find);
    std::string result;
    for (std::size_t start = 0; start < text.size();) {
        std::optional<std::size_t> end;
        if (!options.wholeWord || start == 0 || !word(text[start - 1])) {
            if (!options.wildcard || !pattern.contains('*')) {
                if (std::string_view(text).substr(start).starts_with(pattern)) end = start + pattern.size();
            } else {
                // Dynamic programming bounds wildcard matching; no regex backtracking.
                std::vector<bool> previous(text.size() - start + 1, false);
                previous[0] = true;
                for (char c : pattern) {
                    std::vector<bool> next(previous.size(), false);
                    if (c == '*') next[0] = previous[0];
                    for (std::size_t n = 1; n < next.size(); ++n)
                        next[n] = c == '*' ? previous[n] || next[n - 1] : previous[n - 1] && c == text[start + n - 1];
                    previous = std::move(next);
                }
                for (std::size_t n = 1; n < previous.size(); ++n)
                    if (previous[n] && (!options.wholeWord || start + n == text.size() || !word(text[start + n]))) end = start + n;
            }
        }
        if (end && (!options.wholeWord || *end == text.size() || !word(text[*end]))) {
            result += options.replacement;
            start = *end;
        } else result += original[start++];
    }
    return result;
}
const kv1::Node* scalar(const document::Document& doc, NodeId id, std::size_t index) {
    const auto h = doc.handle(id);
    const auto* node = h ? doc.get(*h) : nullptr;
    if (!node || !node->block || index >= node->block->children().size()) return nullptr;
    const auto& entry = node->block->children()[index];
    return entry.kind() == kv1::NodeKind::Value ? &entry : nullptr;
}
std::string materialName(std::string_view name) {
    auto result = toLowerAscii(name);
    std::ranges::replace(result, '\\', '/');
    return result;
}
}
Result<std::vector<KeyvalueChange>> previewKeyvalueReplace(const document::Document& doc,
    const std::set<NodeId>& entities, const KeyvalueReplaceOptions& options) {
    if (options.find.empty()) return std::unexpected(invalid("find text is empty"));
    std::vector<KeyvalueChange> result;
    for (auto id : entities) {
        const auto h = doc.handle(id);
        if (!h || !std::holds_alternative<Handle<document::Entity>>(*h))
            return std::unexpected(invalid("replace scope contains a stale or non-entity id"));
        const auto* entity = doc.get(*h);
        const auto add = [&](const kv1::Node& block, std::optional<NodeId> connectionBlock) -> Result<void> {
            for (std::size_t i = 0; i < block.children().size(); ++i) {
                const auto& entry = block.children()[i];
                if (entry.kind() != kv1::NodeKind::Value || (!connectionBlock && ciEquals(entry.key(), "id"))) continue;
                const std::string key(entry.key()), oldValue = entry.decodedValue().value_or("");
                auto newKey = options.keys ? replaceText(key, options) : key;
                auto newValue = oldValue;
                if (connectionBlock) {
                    auto connection = document::parseConnection(oldValue);
                    if (!connection.wellFormed) continue;
                    if (options.values) {
                        auto fields = connection.fields();
                        for (std::size_t field = 0; field < fields.size(); ++field) {
                            auto text = replaceText(fields[field], options);
                            if (text != fields[field] && (text.contains(connection.separator) || text.contains('\x1b')))
                                return std::unexpected(invalid("replacement would change connection separators"));
                            connection.setField(static_cast<document::ConnectionField>(field), std::move(text));
                        }
                    }
                    newValue = document::serialize(connection);
                } else if (options.values) newValue = replaceText(oldValue, options);
                if (newKey.empty() || (!connectionBlock && ciEquals(newKey, "id")))
                    return std::unexpected(invalid("replacement would create an empty or identity key"));
                if (key != newKey || oldValue != newValue)
                    result.push_back({id, key, newKey, oldValue, newValue, i, connectionBlock});
            }
            return {};
        };
        if (auto r = add(*entity->block, {}); !r) return std::unexpected(r.error());
        if (options.connections) for (auto child : entity->children) {
            const auto* node = doc.get(*doc.handle(child));
            if (ciEquals(node->block->key(), "connections"))
                if (auto r = add(*node->block, child); !r) return std::unexpected(r.error());
        }
    }
    return result;
}
Result<void> applyKeyvalueReplace(EditorSession& session, std::span<const KeyvalueChange> changes) {
    auto* doc = session.document();
    if (!doc) return std::unexpected(invalid("no document attached"));
    if (session.transformActive()) return std::unexpected(invalid("a transform is active"));
    try {
        auto tx = doc->begin("Replace keyvalues");
        std::map<NodeId, kv1::Node> blocks;
        for (const auto& change : changes) {
            const auto h = doc->handle(change.entity);
            if (!h || !std::holds_alternative<Handle<document::Entity>>(*h)) return std::unexpected(invalid("stale replace entity"));
            const auto* entry = scalar(*doc, change.connectionBlock.value_or(change.entity), change.index);
            if (!entry || entry->key() != change.key || entry->decodedValue() != change.oldValue)
                return std::unexpected(invalid("stale keyvalue preview"));
            if (change.connectionBlock) {
                const auto* node = doc->get(*doc->handle(*change.connectionBlock));
                if (node->parent != change.entity || !ciEquals(node->block->key(), "connections"))
                    return std::unexpected(invalid("stale connection block"));
                auto [it, inserted] = blocks.try_emplace(change.entity, *node->block);
                (void)inserted;
                auto& field = it->second.children()[change.index];
                field.setKey(change.newKey);
                if (auto r = field.setValue(change.newValue); !r) return r;
            } else tx.setEntityKeyValueEntry(*h, change.index, change.newKey, change.newValue);
        }
        for (auto& [id, block] : blocks) tx.setConnections(*doc->handle(id), std::move(block));
        tx.setSelection(session.selection().toBlob(), session.selection().toBlob());
        tx.commit(false);
    } catch (const std::exception& e) { return std::unexpected(invalid(e.what())); }
    return {};
}
Result<std::vector<MaterialChange>> previewMaterialReplace(const EditorSession& session,
    const MaterialReplaceOptions& options) {
    const auto* doc = session.document();
    if (!doc) return std::unexpected(invalid("no document attached"));
    if (options.find.empty()) return std::unexpected(invalid("find material is empty"));
    std::vector<MaterialChange> result;
    std::set<NodeId> selected;
    std::set<SideId> sides;
    if (options.scope == MaterialReplaceScope::Selection) {
        const auto expanded = expandSelection(*doc, session.groups(), session.selection());
        selected.insert(expanded.solidsAndEntities.begin(), expanded.solidsAndEntities.end());
        sides.insert(expanded.sides.begin(), expanded.sides.end());
        // Brush entities expand to solids; retain explicitly selected entity keys too.
        for (const auto& target : session.selection().ordered()) if (const auto* id = std::get_if<NodeId>(&target)) {
            const auto h = doc->handle(*id);
            if (h && std::holds_alternative<Handle<document::Entity>>(*h)) selected.insert(*id);
            if (h && std::holds_alternative<Handle<document::Group>>(*h))
                for (auto member : session.groups().members(*id, true)) selected.insert(member);
        }
    }
    const auto replacement = [&](std::string_view old) {
        if (options.wholeName) return materialName(old) == materialName(options.find) ? options.replacement : std::string(old);
        auto normalized = materialName(old);
        const auto find = materialName(options.find);
        std::string value;
        std::size_t start = 0;
        for (;;) {
            const auto pos = normalized.find(find, start);
            if (pos == std::string::npos) { value += old.substr(start); break; }
            value += old.substr(start, pos - start);
            value += options.replacement;
            start = pos + find.size();
        }
        return value;
    };
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto h = *doc->handle(id);
        const auto* node = doc->get(h);
        const bool inScope = options.scope == MaterialReplaceScope::WholeMap || selected.contains(id);
        if (std::holds_alternative<Handle<document::Solid>>(h)) {
            for (const auto& side : node->block->children()) if (ciEquals(side.key(), "side")) {
                const auto* sid = side.find("id");
                const auto* material = side.find("material");
                if (!sid || !material) continue;
                const auto parsedId = sid->asInt();
                if (!parsedId) continue;
                const auto sideId = *parsedId;
                if (!inScope && !sides.contains(sideId)) continue;
                const auto old = material->decodedValue().value_or("");
                const auto next = replacement(old);
                if (next != old) result.push_back({id, sideId, "material", old, next, 0});
            }
        } else if (options.entityKeys && inScope && std::holds_alternative<Handle<document::Entity>>(h)) {
            for (std::size_t i = 0; i < node->block->children().size(); ++i) {
                const auto& entry = node->block->children()[i];
                if (entry.kind() != kv1::NodeKind::Value || entityAssetKind(*node->block, entry.key(), &session.entityClasses()) != AssetKind::Material) continue;
                const auto old = entry.decodedValue().value_or("");
                const auto next = replacement(old);
                if (next != old) result.push_back({id, {}, std::string(entry.key()), old, next, i});
            }
        }
        for (auto child : node->children) self(self, child);
    };
    for (auto root : doc->roots()) walk(walk, root);
    return result;
}
Result<void> applyMaterialReplace(EditorSession& session, std::span<const MaterialChange> changes) {
    auto* doc = session.document();
    if (!doc) return std::unexpected(invalid("no document attached"));
    if (session.transformActive()) return std::unexpected(invalid("a transform is active"));
    try {
        auto tx = doc->begin("Replace materials");
        for (const auto& change : changes) {
            if (change.side) {
                const auto location = doc->side(*change.side);
                if (!location || doc->get(location->solid)->id != change.object) return std::unexpected(invalid("stale material face"));
                const auto* solid = doc->get(location->solid);
                const auto& side = solid->block->children();
                const kv1::Node* entry = nullptr;
                for (const auto& child : side) if (ciEquals(child.key(), "side")) {
                    const auto* id = child.find("id");
                    if (id && id->decodedValue() == std::to_string(*change.side)) { entry = child.find("material"); break; }
                }
                if (!entry || entry->decodedValue() != change.oldValue) return std::unexpected(invalid("stale material preview"));
                tx.setSideKeyValue(*change.side, "material", change.newValue);
            } else {
                const auto* entry = scalar(*doc, change.object, change.index);
                if (!entry || entry->key() != change.key || entry->decodedValue() != change.oldValue)
                    return std::unexpected(invalid("stale material key preview"));
                tx.setEntityKeyValueEntry(*doc->handle(change.object), change.index, change.key, change.newValue);
            }
        }
        tx.setSelection(session.selection().toBlob(), session.selection().toBlob());
        tx.commit(false);
    } catch (const std::exception& e) { return std::unexpected(invalid(e.what())); }
    return {};
}
} // namespace apex::editor
