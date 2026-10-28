#include "editor/incoming_connections.hpp"

#include <algorithm>
#include <set>

namespace apex::editor {

std::vector<IncomingConnection> incomingConnections(const ConnectionValidator& validator,
                                                     const document::Document& doc,
                                                     std::span<const document::NodeId> targets) {
    std::set<document::NodeId> targetSet(targets.begin(), targets.end());
    std::vector<IncomingConnection> result;
    const auto visit = [&](const auto& self, document::NodeId id) -> void {
        const auto handle = doc.handle(id);
        const auto* node = handle ? doc.get(*handle) : nullptr;
        if (!node) return;
        if (handle && std::holds_alternative<Handle<document::Entity>>(*handle) && node->block) {
            for (const auto childId : node->children) {
                const auto childHandle = doc.handle(childId);
                const auto* child = childHandle ? doc.get(*childHandle) : nullptr;
                if (!child || !child->block || !ciEquals(child->block->key(), "connections")) continue;
                for (std::size_t index = 0; index < child->block->children().size(); ++index) {
                    const auto& entry = child->block->children()[index];
                    if (entry.kind() != kv1::NodeKind::Value) continue;
                    auto connection = document::parseConnection(entry.decodedValue().value_or(""));
                    if (!connection.wellFormed || isSpecialTarget(connection.target)) continue;
                    const auto matches = validator.matching(connection.target);
                    if (std::ranges::any_of(matches, [&](auto match) { return targetSet.contains(match); }))
                        result.push_back({id, index, std::string(entry.key()), std::move(connection)});
                }
            }
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) visit(visit, root);
    return result;
}

} // namespace apex::editor
