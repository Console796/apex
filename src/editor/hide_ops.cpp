#include "editor/hide_ops.hpp"

#include "core/strings.hpp"
#include "editor/visibility.hpp"

#include <algorithm>
#include <set>

namespace apex::editor {
namespace {

struct Location {
    NodeId node, parent;
    std::size_t position;
};
std::vector<Location> hierarchy(const document::Document& doc) {
    std::vector<Location> result;
    const auto walk = [&](const auto& self, const std::vector<NodeId>& children, NodeId parent) -> void {
        for (std::size_t i = 0; i < children.size(); ++i) {
            const auto id = children[i];
            result.push_back({id, parent, i});
            self(self, doc.get(*doc.handle(id))->children, id);
        }
    };
    walk(walk, doc.roots(), 0);
    return result;
}
bool solid(const document::Document& doc, NodeId id) {
    return std::holds_alternative<Handle<document::Solid>>(*doc.handle(id));
}
bool entity(const document::Document& doc, NodeId id) {
    const auto h = *doc.handle(id);
    return std::holds_alternative<Handle<document::Entity>>(h) &&
           !ciEquals(doc.get(h)->block->key(), "world");
}
EditedObjects::Item item(const document::Document& doc, NodeId id) {
    return {id, solid(doc, id) ? EditedObjects::Item::Kind::Solid : EditedObjects::Item::Kind::Entity,
            solid(doc, id) ? std::vector<NodeId>{id} : solidsOf(doc, id), {}};
}
EditedObjects hide(const document::Document& doc, const std::vector<NodeId>& targets,
                   document::Transaction& tx) {
    std::set<NodeId> wrap;
    EditedObjects result;
    for (const auto id : targets) {
        if (!isVisible(doc, id)) continue;
        result.items.push_back(item(doc, id));
        wrap.insert(id);
        if (entity(doc, id)) {
            for (const auto child : solidsOf(doc, id)) {
                if (!isHiddenByWrapper(doc, child)) wrap.insert(child);
            }
        }
    }
    const auto locations = hierarchy(doc);
    for (const auto& loc : locations) {
        if (!wrap.contains(loc.node)) continue;
        const auto parent = loc.parent ? doc.handle(loc.parent) : std::nullopt;
        const auto wrapper = tx.create(kv1::Node::block("hidden"), parent);
        tx.reparent(wrapper, parent, loc.position);
        tx.reparent(*doc.handle(loc.node), wrapper);
    }
    return result;
}

} // namespace

std::vector<NodeId> visibilityTargets(const document::Document& doc, const GroupIndex& groups,
                                const Selection& selection) {
    std::set<NodeId> targets;
    const auto add = [&](NodeId id) {
        if (!doc.handle(id)) return;
        if (solid(doc, id) || entity(doc, id)) targets.insert(id);
    };
    for (const auto& target : selection.ordered()) {
        if (const auto* face = std::get_if<document::SideId>(&target)) {
            if (const auto side = doc.side(*face)) add(doc.get(side->solid)->id);
            continue;
        }
        const auto id = std::get<NodeId>(target);
        if (!doc.handle(id)) continue;
        if (std::holds_alternative<Handle<document::Group>>(*doc.handle(id))) {
            for (const auto member : groups.members(id, true)) add(member);
        } else add(id);
    }
    return {targets.begin(), targets.end()};
}
Result<EditedObjects> stageHide(const document::Document& doc, const GroupIndex& groups,
                                const Selection& selection, document::Transaction& tx) {
    auto targets = visibilityTargets(doc, groups, selection);
    if (selection.mode() != SelectionMode::Faces) {
        for (auto& id : targets) {
            if (solid(doc, id)) {
                if (const auto owner = brushEntityOf(doc, id)) id = *owner;
            }
        }
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    }
    return hide(doc, targets, tx);
}
Result<EditedObjects> stageHideUnselected(const document::Document& doc, const GroupIndex& groups,
                                          const Selection& selection, document::Transaction& tx) {
    const auto selected = visibilityTargets(doc, groups, selection);
    std::set<NodeId> keep(selected.begin(), selected.end());
    // Selected brush solids protect their top-level owner.
    for (const auto id : selected) {
        if (solid(doc, id)) {
            if (const auto owner = brushEntityOf(doc, id)) keep.insert(*owner);
        }
    }
    std::vector<NodeId> targets;
    for (const auto& loc : hierarchy(doc)) {
        const auto id = loc.node;
        if (!keep.contains(id) && (entity(doc, id) || (solid(doc, id) && !brushEntityOf(doc, id))))
            targets.push_back(id);
    }
    return hide(doc, targets, tx);
}
Result<EditedObjects> stageUnhideAll(const document::Document& doc, document::Transaction& tx) {
    const auto locations = hierarchy(doc);
    std::set<NodeId> unhidden;
    for (const auto& loc : locations) {
        if ((solid(doc, loc.node) || entity(doc, loc.node)) && !isVisible(doc, loc.node))
            unhidden.insert(loc.node);
    }
    // Descendants first, siblings last-to-first: original wrapper positions stay valid.
    for (auto it = locations.rbegin(); it != locations.rend(); ++it) {
        const auto h = *doc.handle(it->node);
        const auto* node = doc.get(h);
        if (ciEquals(node->block->key(), "hidden")) {
            const auto children = node->children;
            auto position = it->position;
            for (const auto child : children) tx.reparent(*doc.handle(child), doc.handle(it->parent), position++);
            tx.destroy(h);
        } else if (solid(doc, it->node) || std::holds_alternative<Handle<document::Entity>>(h) ||
                   std::holds_alternative<Handle<document::Group>>(h)) {
            const auto reset = [&](const kv1::Node& editor) {
                for (const auto key : {"visgroupshown", "visgroupautoshown"}) {
                    if (const auto* value = editor.find(key); value && value->decodedValue() == "0") {
                        tx.setEditor(h, ciEquals(key, "visgroupshown") ?
                            document::EditorEdit::setVisgroupShown(true) : document::EditorEdit::setVisgroupAutoShown(true));
                    }
                }
            };
            // Keep a snapshot: setEditor can invalidate the node and its inline block.
            const auto block = node->block;
            const auto children = node->children;
            for (const auto& child : block->children()) {
                if (ciEquals(child.key(), "editor")) reset(child);
            }
            for (const auto child : children) {
                const auto editor = doc.get(*doc.handle(child))->block;
                if (ciEquals(editor->key(), "editor")) reset(*editor);
            }
        }
    }
    EditedObjects result;
    GroupIndex groups(doc);
    for (const auto id : unhidden) {
        if (!isVisible(doc, id)) continue;
        if (solid(doc, id)) {
            if (const auto owner = brushEntityOf(doc, id); owner && unhidden.contains(*owner)) continue;
        }
        auto object = item(doc, id);
        object.group = groups.parentGroup(id);
        result.items.push_back(std::move(object));
        for (auto group = groups.parentGroup(id); group; group = groups.parentGroup(*group))
            result.groupParents[*group] = groups.parentGroup(*group);
    }
    return result;
}

} // namespace apex::editor
