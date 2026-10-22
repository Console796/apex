#include "editor/group_ops.hpp"

#include "core/strings.hpp"
#include "editor/selection_expand.hpp"

#include <algorithm>

namespace apex::editor {
namespace {

using document::EditorEdit;
using Kind = EditedObjects::Item::Kind;

const document::Node* nodeOf(const document::Document& doc, NodeId id) {
    const auto h = doc.handle(id);
    return h ? doc.get(*h) : nullptr;
}
template <typename T> bool is(const document::Document& doc, NodeId id) {
    const auto h = doc.handle(id);
    return h && std::holds_alternative<Handle<T>>(*h);
}
bool isWorld(const document::Document& doc, NodeId id) {
    const auto* node = nodeOf(doc, id);
    return is<document::Entity>(doc, id) && node->block && ciEquals(node->block->key(), "world");
}
bool isEntity(const document::Document& doc, NodeId id) {
    return is<document::Entity>(doc, id) && !isWorld(doc, id);
}
bool isSolid(const document::Document& doc, NodeId id) { return is<document::Solid>(doc, id); }
bool isGroup(const document::Document& doc, NodeId id) { return is<document::Group>(doc, id); }

// The non-world entity that owns a solid, through structural wrappers such as hidden.
std::optional<NodeId> brushOwner(const document::Document& doc, NodeId solid) {
    const auto* node = nodeOf(doc, solid);
    for (auto parent = node ? node->parent : 0; parent;) {
        if (isEntity(doc, parent)) return parent;
        if (isWorld(doc, parent)) return std::nullopt;
        const auto* next = nodeOf(doc, parent);
        parent = next ? next->parent : 0;
    }
    return std::nullopt;
}

EditedObjects::Item makeItem(const document::Document& doc, NodeId node, std::optional<NodeId> group) {
    EditedObjects::Item item;
    item.node = node;
    item.group = group;
    if (isGroup(doc, node)) item.kind = Kind::Group;
    else if (isSolid(doc, node)) {
        item.kind = Kind::Solid;
        item.solids = {node};
    } else {
        item.kind = Kind::Entity;
        item.solids = solidsOf(doc, node);
    }
    return item;
}

void addChain(EditedObjects& objects, const GroupIndex& groups, std::optional<NodeId> group) {
    while (group) {
        const auto parent = groups.parentGroup(*group);
        objects.groupParents[*group] = parent;
        group = parent;
    }
}

bool hasAncestorIn(const document::Document& doc, NodeId id, const std::set<NodeId>& ids) {
    const auto* node = nodeOf(doc, id);
    for (auto parent = node ? node->parent : 0; parent;) {
        if (ids.contains(parent)) return true;
        const auto* next = nodeOf(doc, parent);
        parent = next ? next->parent : 0;
    }
    return false;
}

void destroyAll(const document::Document& doc, document::Transaction& tx, const std::set<NodeId>& ids) {
    std::vector<document::NodeHandle> handles;
    for (const auto id : ids) {
        if (hasAncestorIn(doc, id, ids)) continue; // Destroy is recursive.
        if (const auto h = doc.handle(id)) handles.push_back(*h);
    }
    for (const auto h : handles) tx.destroy(h);
}

// Adds to `removed` the brush entities and groups that the removal empties. Groups in
// `protectedGroups` keep their (new) members and survive. Membership is read from the index,
// which still describes the document before the edit.
void cascadeEmpty(const document::Document& doc, const GroupIndex& groups, std::set<NodeId>& removed,
                  const std::set<NodeId>& oldEntities, const std::set<NodeId>& protectedGroups) {
    for (const auto entity : oldEntities) {
        if (removed.contains(entity)) continue;
        const auto solids = solidsOf(doc, entity);
        if (std::ranges::all_of(solids, [&](NodeId s) { return removed.contains(s); })) removed.insert(entity);
    }
    for (bool changed = true; changed;) {
        changed = false;
        std::set<NodeId> candidates;
        for (const auto node : removed) {
            if (const auto parent = groups.parentGroup(node)) candidates.insert(*parent);
        }
        for (const auto group : candidates) {
            if (removed.contains(group) || protectedGroups.contains(group)) continue;
            const auto members = groups.members(group, false);
            if (std::ranges::all_of(members, [&](NodeId m) { return removed.contains(m); })) {
                removed.insert(group);
                changed = true;
            }
        }
    }
}

} // namespace

std::optional<NodeId> brushEntityOf(const document::Document& doc, NodeId solid) {
    return brushOwner(doc, solid);
}

std::string groupColor(int vmfId) {
    static const char* const colors[] = {"0 190 80", "90 130 255", "255 160 40", "220 80 200",
                                         "60 200 220", "230 220 60"};
    return colors[static_cast<unsigned>(vmfId) % 6];
}

std::optional<document::NodeHandle> worldHandle(const document::Document& doc) {
    for (const auto root : doc.roots()) {
        if (isWorld(doc, root)) return doc.handle(root);
    }
    return std::nullopt;
}

std::optional<int> vmfId(const document::Document& doc, NodeId node) {
    const auto* n = nodeOf(doc, node);
    const auto* value = n && n->block ? n->block->find("id") : nullptr;
    if (!value) return std::nullopt;
    const auto number = value->asInt();
    return number ? std::optional(*number) : std::nullopt;
}

std::vector<NodeId> solidsOf(const document::Document& doc, NodeId node) {
    std::vector<NodeId> result;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto* n = nodeOf(doc, id);
        if (!n) return;
        if (isSolid(doc, id)) {
            result.push_back(id);
            return;
        }
        for (const auto child : n->children) self(self, child);
    };
    if (const auto* n = nodeOf(doc, node)) {
        for (const auto child : n->children) walk(walk, child);
    }
    return result;
}

std::vector<NodeId> selectedNodes(const Selection& selection) {
    std::vector<NodeId> result;
    for (const auto& target : selection.ordered()) {
        if (const auto* id = std::get_if<NodeId>(&target)) result.push_back(*id);
    }
    return result;
}

Selection selectionFor(const Selection& current, const EditedObjects& objects) {
    const auto mode = current.mode() == SelectionMode::Faces ? SelectionMode::Objects : current.mode();
    Selection next(mode);
    next.setIgnoreGroups(current.ignoreGroups());
    const bool grouped = mode == SelectionMode::Groups && !current.ignoreGroups();
    std::vector<Target> targets;
    std::set<NodeId> seen;
    const auto push = [&](NodeId id) {
        if (seen.insert(id).second) targets.emplace_back(id);
    };
    for (const auto& item : objects.items) {
        if (grouped) {
            auto top = item.group;
            while (top) {
                const auto parent = objects.groupParents.find(*top);
                if (parent == objects.groupParents.end() || !parent->second) break;
                top = parent->second;
            }
            push(top ? *top : item.node);
        } else if (mode == SelectionMode::Solids) {
            for (const auto solid : item.solids) push(solid);
        } else if (item.kind != Kind::Group) {
            push(item.node);
        }
    }
    next.replace(targets);
    return next;
}

Result<EditedObjects> stageDelete(const document::Document& doc, const GroupIndex& groups,
                                  const Selection& selection, document::Transaction& tx) {
    std::set<NodeId> removed;
    for (const auto id : selectedNodes(selection)) {
        if (!doc.handle(id)) continue;
        if (isGroup(doc, id)) {
            removed.insert(id);
            for (const auto member : groups.members(id, true)) removed.insert(member);
        } else if (isSolid(doc, id) || isEntity(doc, id)) {
            removed.insert(id);
        }
    }
    if (removed.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "nothing selected to delete"});
    std::set<NodeId> entities;
    for (const auto id : removed) {
        if (!isSolid(doc, id)) continue;
        if (const auto entity = brushOwner(doc, id)) entities.insert(*entity);
    }
    cascadeEmpty(doc, groups, removed, entities, {});
    destroyAll(doc, tx, removed);
    return EditedObjects{};
}

Result<EditedObjects> stageGroup(const document::Document& doc, const GroupIndex& groups,
                                 const Selection& selection, document::Transaction& tx) {
    std::vector<NodeId> items;
    std::set<NodeId> seen;
    for (const auto id : selectedNodes(selection)) {
        if (!doc.handle(id)) continue;
        auto item = id;
        if (isSolid(doc, id)) {
            if (const auto entity = brushOwner(doc, id)) item = *entity;
        } else if (!isEntity(doc, id) && !isGroup(doc, id)) {
            continue;
        }
        if (!selection.ignoreGroups()) {
            if (const auto outer = groups.outermostGroup(item)) item = *outer;
        }
        if (seen.insert(item).second) items.push_back(item);
    }
    if (items.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "nothing selected to group"});

    // Nest inside the members' shared group, if they have one (ignore-groups selections).
    std::optional<NodeId> parent = groups.parentGroup(items.front());
    for (const auto item : items) {
        if (groups.parentGroup(item) != parent) parent.reset();
    }

    auto block = kv1::Node::block("group");
    block.append(kv1::Node::value("id", "0")).value();
    const auto handle = tx.create(std::move(block), worldHandle(doc));
    const auto groupNode = doc.get(handle)->id;
    const int groupId = vmfId(doc, groupNode).value_or(0);
    tx.setEditor(handle, EditorEdit::setColor(groupColor(groupId)));
    if (parent) tx.setEditor(handle, EditorEdit::setGroup(vmfId(doc, *parent)));
    EditedObjects result;
    for (const auto item : items) {
        tx.setEditor(*doc.handle(item), EditorEdit::setGroup(groupId));
        result.items.push_back(makeItem(doc, item, groupNode));
    }
    result.groupParents[groupNode] = parent;
    addChain(result, groups, parent);
    return result;
}

Result<EditedObjects> stageUngroup(const document::Document& doc, const GroupIndex& groups,
                                   const Selection& selection, document::Transaction& tx) {
    std::set<NodeId> dissolve;
    for (const auto id : selectedNodes(selection)) {
        if (!doc.handle(id)) continue;
        if (isGroup(doc, id)) {
            dissolve.insert(id);
            continue;
        }
        auto object = id;
        if (isSolid(doc, id)) {
            if (const auto entity = brushOwner(doc, id)) object = *entity;
        }
        const auto group = selection.ignoreGroups() ? groups.parentGroup(object) : groups.outermostGroup(object);
        if (group) dissolve.insert(*group);
    }
    if (dissolve.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "no groups to ungroup"});

    EditedObjects result;
    for (const auto group : dissolve) {
        auto parent = groups.parentGroup(group);
        while (parent && dissolve.contains(*parent)) parent = groups.parentGroup(*parent);
        const auto parentId = parent ? vmfId(doc, *parent) : std::nullopt;
        for (const auto member : groups.members(group, false)) {
            if (dissolve.contains(member) || !doc.handle(member)) continue;
            tx.setEditor(*doc.handle(member), EditorEdit::setGroup(parentId));
            result.items.push_back(makeItem(doc, member, parent));
            if (isGroup(doc, member)) result.groupParents[member] = parent;
        }
        addChain(result, groups, parent);
    }
    destroyAll(doc, tx, dissolve);
    return result;
}

Result<EditedObjects> stageTieToEntity(const document::Document& doc, const GroupIndex& groups,
                                       const Selection& selection, const TieParams& params,
                                       document::Transaction& tx) {
    std::vector<NodeId> solids;
    for (const auto id : expandSelection(doc, groups, selection).solidsAndEntities) {
        if (isSolid(doc, id)) solids.push_back(id);
    }
    if (solids.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "select solids to tie to an entity"});
    if (params.entity && !isEntity(doc, *params.entity))
        return std::unexpected(Error{ErrorCode::InvalidArgument, "tie target is not an entity"});
    if (!params.entity && trim(params.className).empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "tie needs an entity class"});

    document::NodeHandle target;
    if (params.entity) {
        target = *doc.handle(*params.entity);
    } else {
        auto block = kv1::Node::block("entity");
        block.append(kv1::Node::value("id", "0")).value();
        block.append(kv1::Node::value("classname", params.className)).value();
        for (const auto& [key, value] : params.keyValues) {
            if (ciEquals(key, "classname") || ciEquals(key, "id")) continue;
            if (!block.append(kv1::Node::value(key, value))) continue;
        }
        target = tx.create(std::move(block));
    }
    const auto targetNode = doc.get(target)->id;

    std::set<NodeId> moved, oldEntities, protectedGroups;
    std::optional<NodeId> common;
    bool first = true, uniform = true;
    for (const auto solid : solids) {
        const auto old = brushOwner(doc, solid);
        if (old == targetNode) continue;
        if (old) oldEntities.insert(*old);
        const auto group = groups.parentGroup(old ? *old : solid);
        if (first) common = group;
        else if (common != group) uniform = false;
        first = false;
        // Brush-entity solids carry no groupid of their own; the entity holds the membership.
        if (!old && group) tx.setEditor(*doc.handle(solid), EditorEdit::setGroup(std::nullopt));
        tx.reparent(*doc.handle(solid), target);
        moved.insert(solid);
    }
    if (moved.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the solids are already in that entity"});
    std::optional<NodeId> entityGroup;
    if (params.entity) {
        entityGroup = groups.parentGroup(targetNode);
    } else if (uniform && common) {
        entityGroup = common;
        tx.setEditor(target, EditorEdit::setGroup(vmfId(doc, *common)));
    }
    if (entityGroup) protectedGroups.insert(*entityGroup);

    auto removed = moved;
    cascadeEmpty(doc, groups, removed, oldEntities, protectedGroups);
    for (const auto id : moved) removed.erase(id);
    destroyAll(doc, tx, removed);

    EditedObjects result;
    result.items.push_back(makeItem(doc, targetNode, entityGroup));
    addChain(result, groups, entityGroup);
    return result;
}

Result<EditedObjects> stageMoveToWorld(const document::Document& doc, const GroupIndex& groups,
                                       const Selection& selection, document::Transaction& tx) {
    std::vector<NodeId> solids;
    for (const auto id : expandSelection(doc, groups, selection).solidsAndEntities) {
        if (isSolid(doc, id) && brushOwner(doc, id)) solids.push_back(id);
    }
    if (solids.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "select brush entity solids to move to the world"});
    const auto world = worldHandle(doc);
    std::set<NodeId> moved, oldEntities, protectedGroups;
    EditedObjects result;
    for (const auto solid : solids) {
        const auto entity = *brushOwner(doc, solid);
        oldEntities.insert(entity);
        const auto group = groups.parentGroup(entity);
        tx.reparent(*doc.handle(solid), world);
        if (group) {
            tx.setEditor(*doc.handle(solid), EditorEdit::setGroup(vmfId(doc, *group)));
            protectedGroups.insert(*group);
        }
        moved.insert(solid);
        result.items.push_back(makeItem(doc, solid, group));
        addChain(result, groups, group);
    }
    auto removed = moved;
    cascadeEmpty(doc, groups, removed, oldEntities, protectedGroups);
    for (const auto id : moved) removed.erase(id);
    destroyAll(doc, tx, removed);
    return result;
}

} // namespace apex::editor
