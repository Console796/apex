#include "editor/visibility.hpp"

#include "core/strings.hpp"

namespace apex::editor {
namespace {

bool editorHidden(const kv1::Node& block) {
    for (const auto key : {"visgroupshown", "visgroupautoshown"}) {
        if (const auto* value = block.find(key); value && value->decodedValue() == "0") return true;
    }
    return false;
}

bool locallyHidden(const document::Document& doc, const document::Node& node) {
    if (!node.block) return false;
    if (ciEquals(node.block->key(), "hidden")) return true;
    for (const auto& child : node.block->children()) {
        if (child.kind() == kv1::NodeKind::Block && ciEquals(child.key(), "editor") &&
            editorHidden(child)) return true;
    }
    for (const auto id : node.children) {
        const auto handle = doc.handle(id);
        const auto* child = handle ? doc.get(*handle) : nullptr;
        if (child && child->block && ciEquals(child->block->key(), "editor") &&
            editorHidden(*child->block)) return true;
    }
    return false;
}

} // namespace

bool isHiddenByWrapper(const document::Document& doc, NodeId id) {
    while (id) {
        const auto handle = doc.handle(id);
        const auto* node = handle ? doc.get(*handle) : nullptr;
        if (!node) return false;
        if (node->block && ciEquals(node->block->key(), "hidden")) return true;
        id = node->parent;
    }
    return false;
}

bool isVisible(const document::Document& doc, NodeId id, const ViewState& view) {
    if (!id) return false;
    bool isolated = !view.isolate;
    while (id) {
        const auto handle = doc.handle(id);
        const auto* node = handle ? doc.get(*handle) : nullptr;
        if (!node || locallyHidden(doc, *node)) return false;
        if (view.isolate && view.isolate->contains(id)) isolated = true;
        id = node->parent;
    }
    return isolated;
}

} // namespace apex::editor
