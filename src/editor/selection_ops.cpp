#include "editor/selection_ops.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <set>

namespace apex::editor {
namespace {

bool world(const document::Node& node) {
    if (!node.block) return false;
    if (ciEquals(node.block->key(), "world")) return true;
    const auto* classname = node.block->find("classname");
    return classname && ciEquals(classname->decodedValue().value_or(""), "worldspawn");
}

std::vector<Target> sides(const document::Document& doc, NodeId id) {
    std::vector<Target> result;
    const auto h = doc.handle(id);
    const auto* n = h ? doc.get(*h) : nullptr;
    if (!n || !n->block || !std::holds_alternative<Handle<document::Solid>>(*h)) return result;
    for (const auto& child : n->block->children()) {
        if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
        if (const auto* value = child.find("id")) {
            if (const auto idValue = value->asInt(); idValue && *idValue >= 0) result.emplace_back(*idValue);
        }
    }
    return result;
}

void apply(Selection& selection, const std::vector<Target>& targets, Modifiers modifiers) {
    if (!modifiers.ctrl && !modifiers.shift) {
        selection.replace(targets);
        return;
    }
    for (const auto target : targets) {
        if (modifiers.ctrl) selection.toggle(target);
        else selection.add(target);
    }
}

std::vector<Target> universe(const Selection& selection, const PickIndex& picks, const GroupIndex& groups) {
    const auto& doc = groups.document();
    std::set<Target> result;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto h = doc.handle(id);
        const auto* n = h ? doc.get(*h) : nullptr;
        if (!n) return;
        if (isVisible(doc, id, picks.viewState())) {
            if (selection.mode() == SelectionMode::Faces) {
                for (const auto target : sides(doc, id)) result.insert(target);
            } else if (const auto target = resolveClick({id, {}, {}, {}, 0}, selection.mode(), groups,
                                                        selection.ignoreGroups())) {
                result.insert(*target);
            }
        }
        for (const auto child : n->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
    return {result.begin(), result.end()};
}

} // namespace

std::optional<Target> resolveClick(const Hit& hit, SelectionMode mode,
                                   const GroupIndex& groups, bool ignoreGroups) {
    const auto& doc = groups.document();
    const auto h = doc.handle(hit.node);
    const auto* node = h ? doc.get(*h) : nullptr;
    if (!node) return std::nullopt;
    const bool solid = std::holds_alternative<Handle<document::Solid>>(*h);
    if (mode == SelectionMode::Faces) {
        if (solid && hit.side) return Target{*hit.side};
        return std::nullopt;
    }
    if (mode == SelectionMode::Solids) return solid ? std::optional(Target{hit.node}) : std::nullopt;
    if (!solid && (!std::holds_alternative<Handle<document::Entity>>(*h) || world(*node))) return std::nullopt;
    NodeId object = hit.node;
    for (auto parent = node->parent; parent;) {
        const auto ph = doc.handle(parent);
        const auto* p = ph ? doc.get(*ph) : nullptr;
        if (!p) break;
        if (std::holds_alternative<Handle<document::Entity>>(*ph) && !world(*p)) object = parent;
        parent = p->parent;
    }
    if (mode == SelectionMode::Groups && !ignoreGroups) {
        if (const auto g = groups.outermostGroup(object)) object = *g;
    }
    return Target{object};
}

void click(Selection& selection, const PickIndex& picks, const GroupIndex& groups,
           Ray ray, Modifiers modifiers, CycleState* cycle, bool sameSpot) {
    std::vector<Target> targets;
    std::set<Target> seen;
    for (const auto& hit : picks.pick(ray)) {
        if (const auto target = resolveClick(hit, selection.mode(), groups, selection.ignoreGroups());
            target && seen.insert(*target).second) targets.push_back(*target);
    }
    std::size_t cursor = 0;
    if (cycle) {
        if (sameSpot && cycle->ray && cycle->candidates == targets && !targets.empty() &&
            cycle->mode == selection.mode() && cycle->ignoreGroups == selection.ignoreGroups()) {
            cursor = (cycle->cursor + 1) % targets.size();
            for (std::size_t i = 0; i < targets.size() && selection.contains(targets[cursor]); ++i) {
                cursor = (cursor + 1) % targets.size();
            }
        }
        *cycle = {ray, targets, cursor, selection.mode(), selection.ignoreGroups()};
    }
    if (!targets.empty()) apply(selection, {targets[cursor]}, modifiers);
    else if (!modifiers.ctrl && !modifiers.shift) selection.clear();
}

std::optional<Target> hover(const PickIndex& picks, const GroupIndex& groups,
                            Ray ray, SelectionMode mode, bool ignoreGroups) {
    for (const auto& hit : picks.pick(ray)) {
        if (const auto target = resolveClick(hit, mode, groups, ignoreGroups)) return target;
    }
    return std::nullopt;
}

void marquee(Selection& selection, const PickIndex& picks, const GroupIndex& groups,
             std::span<const Plane> frustum, MarqueeMode mode, Modifiers modifiers) {
    const auto inside = picks.frustum(frustum, mode);
    std::set<Target> targets;
    for (const auto id : inside) {
        if (selection.mode() == SelectionMode::Faces) {
            for (const auto side : sides(groups.document(), id)) targets.insert(side);
        } else if (const auto target = resolveClick({id, {}, {}, {}, 0}, selection.mode(), groups,
                                                   selection.ignoreGroups())) targets.insert(*target);
    }
    if (mode == MarqueeMode::Enclosed && selection.mode() != SelectionMode::Faces) {
        auto leaves = picks.frustum({}, Mode::Enclosed);
        const auto& doc = groups.document();
        const auto walk = [&](const auto& self, NodeId id) -> void {
            const auto h = doc.handle(id);
            const auto* n = h ? doc.get(*h) : nullptr;
            if (!n) return;
            // Malformed visible brushes are absent from PickIndex. They cannot be
            // certified inside and must still veto an enclosed entity/group.
            if (std::holds_alternative<Handle<document::Solid>>(*h) &&
                isVisible(doc, id, picks.viewState())) leaves.push_back(id);
            for (const auto child : n->children) self(self, child);
        };
        for (const auto root : doc.roots()) walk(walk, root);
        for (const auto id : leaves) {
            if (std::ranges::binary_search(inside, id)) continue;
            if (const auto target = resolveClick({id, {}, {}, {}, 0}, selection.mode(), groups,
                                                selection.ignoreGroups())) targets.erase(*target);
        }
    }
    apply(selection, {targets.begin(), targets.end()}, modifiers);
}

void selectAll(Selection& selection, const PickIndex& picks, const GroupIndex& groups) {
    selection.replace(universe(selection, picks, groups));
}
void selectNone(Selection& selection) { selection.clear(); }
void invert(Selection& selection, const PickIndex& picks, const GroupIndex& groups) {
    auto targets = universe(selection, picks, groups);
    std::erase_if(targets, [&](Target target) { return selection.contains(target); });
    selection.replace(targets);
}

} // namespace apex::editor
