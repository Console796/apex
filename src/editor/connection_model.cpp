#include "editor/connection_model.hpp"

#include "editor/group_ops.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <set>

namespace apex::editor {
namespace {
const kv1::Node* connections(const document::Document& doc, NodeId entity) {
    const auto h = doc.handle(entity);
    if (!h) return nullptr;
    for (const auto child : doc.get(*h)->children) {
        const auto* node = doc.get(*doc.handle(child));
        if (ciEquals(node->block->key(), "connections")) return node->block.get();
    }
    return nullptr;
}
Result<void> invalid(std::string text) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(text)});
}
bool matches(const ConnectionModel::Row& row, const kv1::Node& entry) {
    return entry.kind() == kv1::NodeKind::Value && entry.key() == row.output &&
           document::parseConnection(entry.decodedValue().value_or("")).fields() == row.connection.fields();
}
} // namespace

ConnectionModel::ConnectionModel(const document::Document& doc, const Selection& selection) : doc_(doc) {
    GroupIndex groups(doc);
    std::set<NodeId> visited;
    const auto select = [&](const auto& self, NodeId id) -> void {
        if (!visited.insert(id).second) return;
        const auto h = doc.handle(id);
        if (!h) return;
        if (std::holds_alternative<Handle<document::Entity>>(*h)) entities_.push_back(id);
        else if (std::holds_alternative<Handle<document::Group>>(*h)) {
            for (const auto member : groups.members(id)) self(self, member);
        } else if (std::holds_alternative<Handle<document::Solid>>(*h)) {
            if (const auto owner = brushEntityOf(doc, id)) self(self, *owner);
        }
    };
    for (const auto id : selectedNodes(selection)) select(select, id);
    if (entities_.empty()) return;
    const auto* first = connections(doc, entities_.front());
    if (!first) return;
    for (std::size_t i = 0; i < first->children().size(); ++i) {
        const auto& entry = first->children()[i];
        if (entry.kind() != kv1::NodeKind::Value) continue;
        Row row{std::string(entry.key()), document::parseConnection(entry.decodedValue().value_or("")), i, {i}};
        if (entities_.size() > 1 && std::ranges::any_of(rows_, [&](const auto& previous) {
            return previous.output == row.output && previous.connection.fields() == row.connection.fields();
        })) continue;
        for (std::size_t e = 1; e < entities_.size(); ++e) {
            const auto* other = connections(doc, entities_[e]);
            if (!other) break;
            const auto found = std::ranges::find_if(other->children(), [&](const auto& child) { return matches(row, child); });
            if (found == other->children().end()) break;
            row.indices.push_back(static_cast<std::size_t>(found - other->children().begin()));
        }
        if (row.indices.size() == entities_.size()) rows_.push_back(std::move(row));
    }
}
Result<void> ConnectionModel::edit(document::Transaction& tx, std::optional<std::size_t> row,
                                  const std::function<bool(kv1::Node&, std::size_t)>& stage,
                                  std::optional<std::size_t> after) const {
    if (entities_.empty() || (row && *row >= rows_.size())) return invalid("invalid connection row or selection");
    try {
        for (std::size_t e = 0; e < entities_.size(); ++e) {
            const auto h = doc_.handle(entities_[e]);
            if (!h) return invalid("stale connection entity");
            const auto* existing = connections(doc_, entities_[e]);
            auto block = existing ? *existing : kv1::Node::block("connections");
            const auto index = row ? rows_[*row].indices[e] : after ? rows_[*after].indices[e] + 1 : block.children().size();
            if (row && (index >= block.children().size() || !matches(rows_[*row], block.children()[index])))
                return invalid("stale connection row");
            if (stage(block, index)) tx.setConnections(*h, std::move(block));
        }
    } catch (const std::exception& error) { return invalid(error.what()); }
    return {};
}
Result<void> ConnectionModel::setField(document::Transaction& tx, std::size_t row, Field field, std::string text) const {
    return edit(tx, row, [&](auto& block, auto index) {
        auto& entry = block.children()[index];
        auto connection = document::parseConnection(entry.decodedValue().value());
        connection.setField(field, text);
        const auto value = document::serialize(connection);
        if (value == connection.raw) return false;
        entry.setValue(value).value();
        return true;
    });
}
Result<void> ConnectionModel::setOutput(document::Transaction& tx, std::size_t row, std::string name) const {
    return edit(tx, row, [&](auto& block, auto index) {
        if (block.children()[index].key() == name) return false;
        block.children()[index].setKey(name);
        return true;
    });
}
Result<void> ConnectionModel::add(document::Transaction& tx, std::string output, document::Connection connection,
                                  std::optional<std::size_t> after) const {
    if (after && *after >= rows_.size()) return invalid("invalid connection insertion row");
    return edit(tx, {}, [&](auto& block, auto index) {
        block.insert(index, kv1::Node::value(output, document::serialize(connection))).value();
        return true;
    }, after);
}
Result<void> ConnectionModel::remove(document::Transaction& tx, std::size_t row) const {
    return edit(tx, row, [](auto& block, auto index) { block.remove(index).value(); return true; });
}
Result<void> ConnectionModel::duplicate(document::Transaction& tx, std::size_t row) const {
    return edit(tx, row, [](auto& block, auto index) {
        auto entry = block.children()[index];
        block.insert(index + 1, std::move(entry)).value();
        return true;
    });
}

} // namespace apex::editor
