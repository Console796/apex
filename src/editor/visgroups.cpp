#include "editor/visgroups.hpp"

#include "core/strings.hpp"
#include "editor/visibility.hpp"

#include <algorithm>
#include <array>

namespace apex::editor {
namespace {
std::unexpected<Error> missing() {
    return std::unexpected(Error{ErrorCode::NotFound, "unknown visgroup"});
}
std::vector<NodeId> hierarchy(const document::Document& doc) {
    std::vector<NodeId> result;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        result.push_back(id);
        for (const auto child : doc.get(*doc.handle(id))->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
    return result;
}
bool object(const document::Document& doc, NodeId id) {
    const auto h = *doc.handle(id);
    return std::holds_alternative<Handle<document::Solid>>(h) ||
        std::holds_alternative<Handle<document::Group>>(h) ||
        (std::holds_alternative<Handle<document::Entity>>(h) &&
         !ciEquals(doc.get(h)->block->key(), "world"));
}
const kv1::Node* editorBlock(const document::Document& doc, NodeId id) {
    const auto* n = doc.get(*doc.handle(id));
    for (const auto& child : n->block->children())
        if (ciEquals(child.key(), "editor")) return &child;
    for (const auto child : n->children) {
        const auto* b = doc.get(*doc.handle(child))->block.get();
        if (ciEquals(b->key(), "editor")) return b;
    }
    return nullptr;
}
bool marked(const document::Document& doc, NodeId id, std::string_view key) {
    const auto* b = editorBlock(doc, id);
    const auto* v = b ? b->find(key) : nullptr;
    return v && v->decodedValue() == "0";
}
std::string value(const kv1::Node& block, std::string_view key) {
    const auto* v = block.find(key);
    return v ? v->decodedValue().value_or("") : "";
}
std::string classname(const document::Document& doc, NodeId id) {
    return value(*doc.get(*doc.handle(id))->block, "classname");
}
bool entity(const document::Document& doc, NodeId id) {
    return object(doc, id) && std::holds_alternative<Handle<document::Entity>>(*doc.handle(id));
}
bool materials(const document::Document& doc, NodeId id, bool nodraw) {
    if (!std::holds_alternative<Handle<document::Solid>>(*doc.handle(id))) return false;
    bool any = false;
    for (const auto& side : doc.get(*doc.handle(id))->block->children()) {
        if (!ciEquals(side.key(), "side")) continue;
        any = true;
        const auto material = value(side, "material");
        if (nodraw && !ciEquals(material, "tools/toolsnodraw")) return false;
        if (!nodraw && material.size() >= 6 && ciEquals(std::string_view(material).substr(0, 6), "tools/")) return true;
    }
    return nodraw && any;
}
const std::array<AutoVisgroup, 7> automatic{{
    {"tool-brushes", "Tool brushes", [](const auto& d, NodeId n) { return materials(d, n, false); }},
    {"point-entities", "Point entities", [](const auto& d, NodeId n) { return entity(d, n) && solidsOf(d, n).empty(); }},
    {"brush-entities", "Brush entities", [](const auto& d, NodeId n) { return entity(d, n) && !solidsOf(d, n).empty(); }},
    {"lights", "Lights", [](const auto& d, NodeId n) { return entity(d, n) && classname(d, n).starts_with("light"); }},
    {"nodraw", "Nodraw", [](const auto& d, NodeId n) { return materials(d, n, true); }},
    {"triggers", "Triggers", [](const auto& d, NodeId n) { return entity(d, n) && classname(d, n).starts_with("trigger_"); }},
    {"detail", "Detail", [](const auto& d, NodeId n) { return entity(d, n) && classname(d, n) == "func_detail"; }}
}};
const AutoVisgroup* findAuto(std::string_view id) {
    for (const auto& a : automatic) if (a.id == id) return &a;
    return nullptr;
}
NodeId root(const document::Document& doc) {
    for (const auto id : doc.roots())
        if (ciEquals(doc.get(*doc.handle(id))->block->key(), "visgroups")) return id;
    return 0;
}

void applyVisibility(const document::Document& doc, VisgroupVisibility& state,
                     document::Transaction& tx) {
    const auto tree = buildVisgroupTree(doc);
    const auto all = hierarchy(doc);
    std::map<NodeId, std::pair<bool, bool>> reasons;
    std::set<NodeId> needed;
    for (const auto id : all) {
        const auto* n = doc.get(*doc.handle(id));
        auto reason = reasons[n->parent];
        // Only brush entities propagate flags to their solids, through hidden wrappers.
        if (object(doc, id)) {
            for (const auto group : tree.visgroupsOf(id)) reason.first |= state.user.contains(group);
            for (const auto& a : automatic)
                if (state.automatic.contains(a.id) && a.matches(doc, id)) reason.second = true;
            const bool oldUser = marked(doc, id, "visgroupshown");
            const bool oldAuto = marked(doc, id, "visgroupautoshown");
            if (reason.first || oldUser) tx.setEditor(*doc.handle(id), document::EditorEdit::setVisgroupShown(!reason.first));
            if (reason.second || oldAuto) tx.setEditor(*doc.handle(id), document::EditorEdit::setVisgroupAutoShown(!reason.second));
            if (reason.first || reason.second) needed.insert(id);
        }
        reasons[id] = reason;
    }
    // Remove only wrappers owned by visgroup operations. Plain Hide wrappers survive.
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (!state.wrappers.contains(*it)) continue;
        const auto* n = doc.get(*doc.handle(*it));
        bool keep = false;
        for (const auto child : n->children) if (needed.contains(child)) keep = true;
        if (keep) continue;
        const auto parent = n->parent;
        const auto children = n->children;
        const auto siblings = parent ? doc.get(*doc.handle(parent))->children : doc.roots();
        auto pos = static_cast<std::size_t>(std::find(siblings.begin(), siblings.end(), *it) - siblings.begin());
        for (const auto child : children) tx.reparent(*doc.handle(child), doc.handle(parent), pos++);
        tx.destroy(*doc.handle(*it));
        state.wrappers.erase(*it);
    }
    for (const auto id : all) {
        if (!needed.contains(id)) continue;
        const auto* n = doc.get(*doc.handle(id));
        const auto parent = n->parent;
        // An existing plain Hide wrapper already hides this object. Do not nest
        // another wrapper inside it: srctools/Hammer require object children here.
        // Leave its ownership alone so showing the visgroup preserves the plain hide.
        if (parent && ciEquals(doc.get(*doc.handle(parent))->block->key(), "hidden")) continue;
        const auto siblings = parent ? doc.get(*doc.handle(parent))->children : doc.roots();
        const auto pos = static_cast<std::size_t>(std::find(siblings.begin(), siblings.end(), id) - siblings.begin());
        const auto wrapper = tx.create(kv1::Node::block("hidden"), doc.handle(parent));
        state.wrappers.insert(doc.get(wrapper)->id);
        tx.reparent(wrapper, doc.handle(parent), pos);
        tx.reparent(*doc.handle(id), wrapper);
    }
}
} // namespace

const VisgroupNode* VisgroupTree::find(int id) const {
    for (const auto& n : nodes) if (n.vmfId == id) return &n;
    return nullptr;
}
std::vector<NodeId> VisgroupTree::members(int id) const {
    const auto it = membership.find(id);
    return it == membership.end() ? std::vector<NodeId>{} : it->second;
}
std::vector<int> VisgroupTree::visgroupsOf(NodeId node) const {
    const auto it = memberships.find(node);
    return it == memberships.end() ? std::vector<int>{} : it->second;
}
VisgroupTree buildVisgroupTree(const document::Document& doc) {
    VisgroupTree tree;
    std::set<int> defined;
    for (const auto id : hierarchy(doc)) {
        const auto h = *doc.handle(id);
        const auto* n = doc.get(h);
        if (std::holds_alternative<Handle<document::Visgroup>>(h)) {
            const auto* v = n->block->find("visgroupid");
            const auto number = v ? v->asInt().value_or(0) : 0;
            VisgroupNode entry{id, number, value(*n->block, "name"), value(*n->block, "color"), 0, {}};
            if (n->parent && std::holds_alternative<Handle<document::Visgroup>>(*doc.handle(n->parent))) entry.parent = n->parent;
            for (const auto child : n->children)
                if (std::holds_alternative<Handle<document::Visgroup>>(*doc.handle(child))) entry.children.push_back(child);
            tree.nodes.push_back(std::move(entry));
            defined.insert(number);
        }
        if (!object(doc, id)) continue;
        const auto* b = editorBlock(doc, id);
        if (!b) continue;
        std::set<int> seen;
        for (const auto& v : b->children()) {
            if (!ciEquals(v.key(), "visgroupid")) continue;
            const auto number = v.asInt();
            if (!number || !seen.insert(*number).second) continue;
            tree.membership[*number].push_back(id);
            tree.memberships[id].push_back(*number);
            if (marked(doc, id, "visgroupshown")) tree.hidden.insert(*number);
        }
    }
    for (auto& [id, members] : tree.membership) {
        std::ranges::sort(members);
        if (!defined.contains(id)) tree.dangling.insert(id);
    }
    return tree;
}
std::span<const AutoVisgroup> autoVisgroups() { return automatic; }
std::vector<NodeId> autoMembers(const document::Document& doc, std::string_view id) {
    std::vector<NodeId> result;
    if (const auto* a = findAuto(id))
        for (const auto n : hierarchy(doc)) if (a->matches(doc, n)) result.push_back(n);
    std::ranges::sort(result);
    return result;
}
VisgroupVisibility buildVisgroupVisibility(const document::Document& doc) {
    VisgroupVisibility state;
    state.user = buildVisgroupTree(doc).hidden;
    for (const auto id : hierarchy(doc)) {
        if (!object(doc, id)) continue;
        if (marked(doc, id, "visgroupautoshown"))
            for (const auto& a : automatic) if (a.matches(doc, id)) state.automatic.insert(std::string(a.id));
        if (!marked(doc, id, "visgroupshown") && !marked(doc, id, "visgroupautoshown")) continue;
        const auto parent = doc.get(*doc.handle(id))->parent;
        if (parent && ciEquals(doc.get(*doc.handle(parent))->block->key(), "hidden")) state.wrappers.insert(parent);
    }
    return state;
}
Result<int> stageCreateVisgroup(const document::Document& doc, std::string name,
    std::optional<int> parent, document::Transaction& tx) {
    const auto tree = buildVisgroupTree(doc);
    if (parent && !tree.find(*parent)) return missing();
    NodeId container = parent ? tree.find(*parent)->node : root(doc);
    if (!container) {
        std::size_t pos = 0;
        for (const auto id : doc.roots()) {
            if (ciEquals(doc.get(*doc.handle(id))->block->key(), "world")) break;
            ++pos;
        }
        const auto h = tx.create(kv1::Node::block("visgroups"));
        container = doc.get(h)->id;
        tx.reparent(h, {}, pos);
    }
    auto block = kv1::Node::block("visgroup");
    block.append(kv1::Node::value("name", std::move(name))).value();
    const auto h = tx.create(std::move(block), doc.handle(container));
    const auto number = doc.get(h)->block->find("visgroupid")->asInt().value();
    tx.setKeyValue(h, "color", groupColor(number));
    return number;
}
Result<void> stageRenameVisgroup(const document::Document& doc, int id, std::string name, document::Transaction& tx) {
    const auto tree = buildVisgroupTree(doc);
    if (!tree.find(id)) return missing();
    tx.setKeyValue(*doc.handle(tree.find(id)->node), "name", std::move(name));
    return {};
}
Result<void> stageMoveVisgroup(const document::Document& doc, int id, std::optional<int> parent,
    std::size_t position, document::Transaction& tx) {
    const auto tree = buildVisgroupTree(doc);
    if (!tree.find(id) || (parent && !tree.find(*parent))) return missing();
    const auto node = tree.find(id)->node;
    const auto target = parent ? tree.find(*parent)->node : root(doc);
    for (auto ancestor = target; ancestor; ancestor = doc.get(*doc.handle(ancestor))->parent)
        if (ancestor == node) return std::unexpected(Error{ErrorCode::InvalidArgument, "visgroup cycle"});
    tx.reparent(*doc.handle(node), doc.handle(target), position);
    return {};
}
Result<void> stageVisgroupMembership(const document::Document& doc, const GroupIndex& groups,
    const Selection& selection, int id, bool add, document::Transaction& tx) {
    if (!buildVisgroupTree(doc).find(id)) return missing();
    const auto tree = buildVisgroupTree(doc);
    for (const auto node : visibilityTargets(doc, groups, selection)) {
        const auto ids = tree.visgroupsOf(node);
        if (add) {
            if (std::ranges::find(ids, id) == ids.end())
                tx.setEditor(*doc.handle(node), document::EditorEdit::addVisgroup(id));
        } else {
            const auto* editor = editorBlock(doc, node);
            std::size_t count = 0;
            if (editor) for (const auto& entry : editor->children())
                if (ciEquals(entry.key(), "visgroupid") && entry.asInt() == id) ++count;
            for (std::size_t i = 0; i < count; ++i)
                tx.setEditor(*doc.handle(node), document::EditorEdit::removeVisgroup(id));
        }
    }
    return {};
}
Result<void> stageSetVisgroupShown(const document::Document& doc, int id, bool shown,
    VisgroupVisibility& state, document::Transaction& tx) {
    if (!buildVisgroupTree(doc).find(id)) return missing();
    const bool changed = shown ? state.user.erase(id) != 0 : state.user.insert(id).second;
    if (!changed) return {};
    const auto node = buildVisgroupTree(doc).find(id)->node;
    const auto* n = doc.get(*doc.handle(node));
    const auto siblings = doc.get(*doc.handle(n->parent))->children;
    const auto position = static_cast<std::size_t>(std::find(siblings.begin(), siblings.end(), node) - siblings.begin());
    // An overlap may change only editor state; touching the definition records that undo step.
    tx.reparent(*doc.handle(node), doc.handle(n->parent), position);
    applyVisibility(doc, state, tx);
    return {};
}
Result<void> stageSetAutoVisgroupShown(const document::Document& doc, std::string_view id,
    bool shown, VisgroupVisibility& state, document::Transaction& tx) {
    if (!findAuto(id)) return missing();
    const bool changed = shown ? state.automatic.erase(std::string(id)) != 0 : state.automatic.insert(std::string(id)).second;
    if (!changed) return {};
    const auto members = autoMembers(doc, id);
    if (!members.empty() || !doc.roots().empty()) {
        const auto node = members.empty() ? doc.roots().front() : members.front();
        const auto parent = doc.get(*doc.handle(node))->parent;
        const auto siblings = parent ? doc.get(*doc.handle(parent))->children : doc.roots();
        const auto position = static_cast<std::size_t>(std::find(siblings.begin(), siblings.end(), node) - siblings.begin());
        tx.reparent(*doc.handle(node), doc.handle(parent), position);
    }
    applyVisibility(doc, state, tx);
    return {};
}
Result<void> stageDeleteVisgroup(const document::Document& doc, int id,
    VisgroupVisibility& state, document::Transaction& tx) {
    const auto tree = buildVisgroupTree(doc);
    if (!tree.find(id)) return missing();
    const auto target = tree.find(id)->node;
    std::set<NodeId> subtree{target};
    std::vector<int> ids;
    for (const auto& n : tree.nodes) {
        if (subtree.contains(n.parent)) subtree.insert(n.node);
        if (subtree.contains(n.node)) { ids.push_back(n.vmfId); state.user.erase(n.vmfId); }
    }
    applyVisibility(doc, state, tx);
    for (const auto number : ids) {
        for (const auto member : tree.members(number)) {
            const auto* editor = editorBlock(doc, member);
            std::size_t count = 0;
            if (editor) for (const auto& entry : editor->children())
                if (ciEquals(entry.key(), "visgroupid") && entry.asInt() == number) ++count;
            for (std::size_t i = 0; i < count; ++i)
                tx.setEditor(*doc.handle(member), document::EditorEdit::removeVisgroup(number));
        }
    }
    tx.destroy(*doc.handle(target));
    return {};
}
} // namespace apex::editor
