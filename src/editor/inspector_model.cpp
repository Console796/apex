#include "editor/inspector_model.hpp"

#include "editor/group_ops.hpp"

#include <algorithm>
#include <charconv>
#include <set>

namespace apex::editor {
namespace {
Result<void> invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
}
std::optional<std::string> value(const kv1::Node& block, std::string_view key, std::size_t occurrence = 0) {
    for (const auto& child : block.children()) {
        if (child.kind() == kv1::NodeKind::Value && ciEquals(child.key(), key)) {
            if (occurrence-- == 0) return child.decodedValue().value_or("");
        }
    }
    return {};
}
Result<std::uint64_t> flagValue(std::string_view text) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid flags value"});
    return result;
}
} // namespace

InspectorModel::InspectorModel(const document::Document& doc, const Selection& selection,
                               const assets::EntityClassTable& classes, bool raw) : doc_(doc) {
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
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto* node = doc.get(*doc.handle(id));
        if (std::holds_alternative<Handle<document::Entity>>(*doc.handle(id))) {
            for (const auto& child : node->block->children()) {
                if (child.kind() == kv1::NodeKind::Value && ciEquals(child.key(), "targetname")) {
                    auto name = child.decodedValue().value_or("");
                    if (!name.empty()) targets_.push_back(std::move(name));
                }
            }
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
    std::ranges::sort(targets_);
    auto unique = std::ranges::unique(targets_);
    targets_.erase(unique.begin(), unique.end());
    if (entities_.empty()) return;
    const auto blockOf = [&](NodeId id) -> const kv1::Node& { return *doc.get(*doc.handle(id))->block; };
    const auto classOf = [&](NodeId id) { return classes.find(value(blockOf(id), "classname").value_or("")); };
    for (const auto id : entities_) {
        if (const auto* cls = classOf(id)) {
            for (const auto& prop : cls->properties)
                defaults_[{id, toLowerAscii(prop.name)}] = prop.defaultValue;
        }
    }
    const auto append = [&](Row row) {
        bool first = true;
        for (const auto id : entities_) {
            const auto& block = blockOf(id);
            auto text = value(block, row.key, row.occurrence);
            if (row.property) {
                const auto* cls = classOf(id);
                if (!cls) return;
                const auto prop = std::ranges::find_if(cls->properties, [&](const auto& p) { return ciEquals(p.name, row.key); });
                if (prop == cls->properties.end() || prop->type != row.property->type) return;
                row.readOnly = row.readOnly || prop->readOnly;
                if (!text) text = prop->defaultValue;
            } else if (!text) return; // Raw/unknown keys must be present in every entity.
            if (first) row.value = *text;
            else if (row.value != *text) row.mixed = true;
            if (row.section != Section::Raw && block.findAll(row.key).size() > 1) row.readOnly = true;
            first = false;
        }
        rows_.push_back(std::move(row));
    };
    std::set<std::string> declared;
    if (!raw) {
        if (const auto* cls = classOf(entities_.front())) {
            for (const auto& prop : cls->properties) {
                const bool shared = std::ranges::all_of(entities_, [&](NodeId id) {
                    const auto* other = classOf(id);
                    return other && std::ranges::any_of(other->properties, [&](const auto& p) {
                        return ciEquals(p.name, prop.name) && p.type == prop.type;
                    });
                });
                if (!shared) continue;
                declared.insert(toLowerAscii(prop.name));
                append({prop.name, "", prop, Section::Typed, false, prop.readOnly, 0});
            }
        }
    }
    std::map<std::string, std::size_t> occurrences;
    for (const auto& child : blockOf(entities_.front()).children()) {
        if (child.kind() != kv1::NodeKind::Value) continue;
        const auto key = toLowerAscii(child.key());
        const auto occurrence = occurrences[key]++;
        if (!raw && (declared.contains(key) || occurrence > 0)) continue;
        append({std::string(child.key()), "", {}, raw ? Section::Raw : Section::Other,
                false, key == "id", occurrence});
    }
}
std::vector<std::string> InspectorModel::autocomplete(std::size_t row) const {
    if (row >= rows_.size() || !rows_[row].property) return {};
    const auto& type = rows_[row].property->type;
    return type == "target_destination" || type == "target_source" ? targets_ : std::vector<std::string>{};
}
Result<void> InspectorModel::writable(std::size_t row) const {
    if (entities_.empty() || row >= rows_.size()) return invalid("invalid inspector row");
    if (rows_[row].readOnly) return invalid("read-only property; use raw mode for duplicate keys");
    for (const auto id : entities_) if (!doc_.handle(id)) return invalid("stale inspector entity");
    return {};
}
Result<void> InspectorModel::setProperty(document::Transaction& tx, std::size_t row, std::string text) const {
    if (auto ok = writable(row); !ok) return ok;
    const auto& field = rows_[row];
    try {
        for (const auto id : entities_) {
            const auto h = *doc_.handle(id);
            if (field.section == Section::Raw)
                tx.setKeyValueOccurrence(h, field.key, field.occurrence, text);
            else tx.setKeyValue(h, field.key, text);
        }
    } catch (const std::exception& error) { return invalid(error.what()); }
    return {};
}
Result<void> InspectorModel::toggleFlag(document::Transaction& tx, std::size_t row,
                                       std::uint64_t bit, bool enabled) const {
    if (auto ok = writable(row); !ok) return ok;
    const auto& field = rows_[row];
    if (!field.property || field.property->type != "flags" ||
        std::ranges::none_of(field.property->flags, [&](const auto& flag) { return flag.bit == bit; }))
        return invalid("invalid flag");
    std::vector<std::uint64_t> values;
    for (const auto id : entities_) {
        const auto text = value(*doc_.get(*doc_.handle(id))->block, field.key).value_or(defaults_.at({id, toLowerAscii(field.key)}));
        auto bits = flagValue(text);
        if (!bits) return std::unexpected(bits.error());
        values.push_back(enabled ? (*bits | bit) : (*bits & ~bit));
    }
    try {
        for (std::size_t i = 0; i < entities_.size(); ++i)
            tx.setKeyValue(*doc_.handle(entities_[i]), field.key, std::to_string(values[i]));
    } catch (const std::exception& error) { return invalid(error.what()); }
    return {};
}
Result<void> InspectorModel::setChoice(document::Transaction& tx, std::size_t row, std::string text) const {
    if (auto ok = writable(row); !ok) return ok;
    const auto& prop = rows_[row].property;
    if (!prop || prop->type != "choices" ||
        std::ranges::none_of(prop->choices, [&](const auto& choice) { return choice.value == text; }))
        return invalid("invalid choice");
    return setProperty(tx, row, std::move(text));
}
Result<void> InspectorModel::setClass(document::Transaction& tx, std::string classname) const {
    if (classname.empty() || entities_.empty()) return invalid("select entities and a class");
    try {
        for (const auto id : entities_) {
            const auto h = doc_.handle(id);
            if (!h) return invalid("stale inspector entity");
            tx.setKeyValue(*h, "classname", classname);
        }
    } catch (const std::exception& error) { return invalid(error.what()); }
    return {};
}
} // namespace apex::editor
