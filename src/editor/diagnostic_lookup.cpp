#include "editor/diagnostic_lookup.hpp"

#include "core/strings.hpp"

#include <charconv>

namespace apex::editor {
namespace {
bool hasId(const kv1::Node& block, int id) {
    const auto* field = block.find("id");
    if (!field) return false;
    const auto text = field->decodedValue().value_or("");
    int parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return error == std::errc{} && end == text.data() + text.size() && parsed == id;
}
} // namespace

std::optional<document::NodeId> findDiagnosticObject(const document::Document& document, compile::ObjectRef object) {
    if (object.kind == compile::ObjectKind::Side) {
        const auto location = document.side(object.id);
        if (!location) return std::nullopt;
        return document.get(location->solid)->id;
    }
    std::optional<document::NodeId> found;
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        if (found) return;
        const auto handle = document.handle(id);
        if (!handle) return;
        const auto* node = document.get(*handle);
        const bool solid = std::holds_alternative<Handle<document::Solid>>(*handle);
        const bool entity = std::holds_alternative<Handle<document::Entity>>(*handle) &&
                            !ciEquals(node->block->key(), "world");
        if (((object.kind == compile::ObjectKind::Brush && solid) ||
             (object.kind == compile::ObjectKind::Entity && entity)) && hasId(*node->block, object.id)) {
            found = id;
            return;
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : document.roots()) walk(walk, root);
    return found;
}

} // namespace apex::editor
