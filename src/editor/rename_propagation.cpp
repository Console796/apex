#include "editor/rename_propagation.hpp"

#include "core/entity_helpers.hpp"
#include "core/strings.hpp"
#include "document/connection_codec.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace apex::editor {
namespace {
std::string value(const kv1::Node& block, std::string_view key) {
    const auto* field = block.find(key);
    return field ? field->decodedValue().value_or("") : "";
}
bool entityNode(const document::Document& doc, document::NodeId id) {
    const auto handle = doc.handle(id);
    return handle && std::holds_alternative<Handle<document::Entity>>(*handle);
}
bool referenceMatch(std::string_view reference, std::string_view oldName) {
    if (reference.find('*') != std::string_view::npos || ciEquals(reference, "!activator") ||
        ciEquals(reference, "!caller") || ciEquals(reference, "!self") || ciEquals(reference, "!player") ||
        ciEquals(reference, "!picker") || ciEquals(reference, "!speechtarget") || ciEquals(reference, "!enemy") ||
        ciEquals(reference, "!friend") || toLowerAscii(reference).starts_with("instance:")) return false;
    return ciEquals(reference, oldName);
}
std::set<std::string> referenceKeys(const assets::EntityClass* cls) {
    std::set<std::string> result;
    if (!cls) return result;
    for (const auto& property : cls->properties)
        if (ciEquals(property.type, "target_destination") || ciEquals(property.type, "target_name_or_class"))
            result.insert(toLowerAscii(property.name));
    for (const auto& helper : cls->helpers) {
        const auto parsed = assets::parseHelper(helper);
        if (const auto* line = std::get_if<assets::LineHelper>(&parsed)) {
            if (!line->startValue.empty()) result.insert(toLowerAscii(line->startValue));
            if (!line->endValue.empty()) result.insert(toLowerAscii(line->endValue));
        } else if (const auto* cylinder = std::get_if<assets::CylinderHelper>(&parsed)) {
            if (!cylinder->startValue.empty()) result.insert(toLowerAscii(cylinder->startValue));
            if (!cylinder->endValue.empty()) result.insert(toLowerAscii(cylinder->endValue));
        }
    }
    result.erase("targetname");
    return result;
}
std::string label(const kv1::Node& block, document::NodeId id) {
    auto classname = value(block, "classname");
    auto targetname = value(block, "targetname");
    std::string result = classname.empty() ? "entity " + std::to_string(id) : classname;
    if (!targetname.empty()) result += " (" + targetname + ")";
    return result;
}
Result<void> invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
}
} // namespace

RenamePlan planRename(const document::Document& doc, const assets::EntityClassTable& classes,
                      document::NodeId entity, std::string_view newName) {
    RenamePlan plan;
    if (!entityNode(doc, entity)) return plan;
    const auto* renamed = doc.get(*doc.handle(entity));
    if (!renamed || !renamed->block) return plan;
    plan.oldName = value(*renamed->block, "targetname");
    plan.newName = std::string(newName);
    if (plan.oldName.empty() || plan.newName.empty() || plan.oldName == plan.newName) return plan;

    const auto visit = [&](const auto& self, document::NodeId id) -> void {
        if (entityNode(doc, id)) {
            const auto* node = doc.get(*doc.handle(id));
            if (!node || !node->block) return;
            const auto* classValue = node->block->find("classname");
            const auto* cls = classValue ? classes.find(classValue->decodedValue().value_or("")) : nullptr;
            const auto keys = referenceKeys(cls);
            std::map<std::string, std::size_t> occurrences;
            for (const auto childId : node->children) {
                const auto childHandle = doc.handle(childId);
                const auto* child = childHandle ? doc.get(*childHandle) : nullptr;
                if (!child || !child->block || !ciEquals(child->block->key(), "connections")) continue;
                for (std::size_t index = 0; index < child->block->children().size(); ++index) {
                    const auto& connectionEntry = child->block->children()[index];
                    if (connectionEntry.kind() != kv1::NodeKind::Value) continue;
                    const auto connection = document::parseConnection(connectionEntry.decodedValue().value_or(""));
                    if (!connection.wellFormed || !referenceMatch(connection.target, plan.oldName)) continue;
                    plan.changes.push_back({id, RenameChange::Kind::Connection,
                        std::string(connectionEntry.key()) + " #" + std::to_string(index + 1), index,
                        connection.target, plan.newName});
                    plan.entityLabels.try_emplace(id, label(*node->block, id));
                }
            }
            for (const auto& entry : node->block->children()) {
                if (entry.kind() == kv1::NodeKind::Value) {
                    const auto key = toLowerAscii(entry.key());
                    const auto occurrence = occurrences[key]++;
                    if (key != "targetname" && keys.contains(key)) {
                        const auto before = entry.decodedValue().value_or("");
                        if (referenceMatch(before, plan.oldName)) {
                            plan.changes.push_back({id, RenameChange::Kind::Key, std::string(entry.key()), occurrence,
                                                    before, plan.newName});
                            plan.entityLabels.try_emplace(id, label(*node->block, id));
                        }
                    }
                }
            }
        }
        const auto handle = doc.handle(id);
        const auto* node = handle ? doc.get(*handle) : nullptr;
        if (node) for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) visit(visit, root);
    return plan;
}

Result<void> stageRename(const document::Document& doc, const RenamePlan& plan, document::NodeId entity,
                         document::Transaction& tx) {
    if (!entityNode(doc, entity)) return invalid("only entities can be renamed");
    const auto handle = doc.handle(entity);
    if (!handle) return invalid("stale rename entity");
    const auto* renamed = doc.get(*handle);
    if (!renamed || !renamed->block || !ciEquals(value(*renamed->block, "targetname"), plan.oldName))
        return invalid("stale rename plan");
    try {
        tx.setKeyValue(*handle, "targetname", plan.newName.empty() ? std::nullopt : std::optional(plan.newName));
        std::map<document::NodeId, kv1::Node> connectionBlocks;
        for (const auto& change : plan.changes) {
            const auto sourceHandle = doc.handle(change.entity);
            const auto* source = sourceHandle ? doc.get(*sourceHandle) : nullptr;
            if (!source || !source->block || !entityNode(doc, change.entity)) return invalid("stale rename reference entity");
            if (change.kind == RenameChange::Kind::Key) {
                std::size_t occurrence = change.index;
                const kv1::Node* field = nullptr;
                for (const auto& entry : source->block->children()) {
                    if (entry.kind() != kv1::NodeKind::Value || !ciEquals(entry.key(), change.where)) continue;
                    if (occurrence-- == 0) { field = &entry; break; }
                }
                if (!field || !ciEquals(field->decodedValue().value_or(""), change.before))
                    return invalid("stale rename key reference");
                tx.setKeyValueOccurrence(*sourceHandle, change.where, change.index, change.after);
                continue;
            }
            auto found = connectionBlocks.find(change.entity);
            if (found == connectionBlocks.end()) {
                const kv1::Node* block = nullptr;
                for (const auto childId : source->children) {
                    const auto childHandle = doc.handle(childId);
                    const auto* child = childHandle ? doc.get(*childHandle) : nullptr;
                    if (child && child->block && ciEquals(child->block->key(), "connections")) { block = child->block.get(); break; }
                }
                if (!block) return invalid("missing rename connections block");
                found = connectionBlocks.emplace(change.entity, *block).first;
            }
            if (change.index >= found->second.children().size()) return invalid("stale rename connection index");
            auto& entry = found->second.children()[change.index];
            if (entry.kind() != kv1::NodeKind::Value) return invalid("invalid rename connection entry");
            auto connection = document::parseConnection(entry.decodedValue().value_or(""));
            if (!connection.wellFormed || !ciEquals(connection.target, change.before))
                return invalid("stale rename connection reference");
            connection.setField(document::ConnectionField::Target, change.after);
            entry.setValue(document::serialize(connection)).value();
        }
        for (auto& [nodeId, block] : connectionBlocks) tx.setConnections(*doc.handle(nodeId), std::move(block));
    } catch (const std::exception& error) {
        return invalid(error.what());
    }
    return {};
}

std::vector<std::string> describe(const RenamePlan& plan) {
    std::vector<std::string> result;
    result.reserve(plan.changes.size());
    for (const auto& change : plan.changes) {
        const auto label = plan.entityLabels.find(change.entity);
        std::string line = label == plan.entityLabels.end() ? "entity " + std::to_string(change.entity) : label->second;
        line += " " + change.where + ": " + change.before + " -> " + change.after;
        result.push_back(std::move(line));
    }
    return result;
}

} // namespace apex::editor
