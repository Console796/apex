#include "editor/clipboard.hpp"

#include "core/strings.hpp"
#include "document/connection_codec.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace apex::editor {
namespace {

using Kv = kv1::Node;
using IdMap = std::map<int, int>;

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message)}; }

std::optional<int> intOf(const Kv* node) {
    if (!node || node->kind() != kv1::NodeKind::Value) return std::nullopt;
    const auto number = node->asInt();
    return number ? std::optional(*number) : std::nullopt;
}
std::string stringOf(const Kv* parent, std::string_view key) {
    const auto* node = parent ? parent->find(key) : nullptr;
    if (!node || node->kind() != kv1::NodeKind::Value) return {};
    const auto value = node->decodedValue();
    return value ? *value : std::string();
}
bool isBlock(const Kv& node, std::string_view key) {
    return node.kind() == kv1::NodeKind::Block && ciEquals(node.key(), key);
}

template <typename T> bool holds(const document::Document& doc, NodeId id) {
    const auto h = doc.handle(id);
    return h && std::holds_alternative<Handle<T>>(*h);
}
const document::Node* nodeOf(const document::Document& doc, NodeId id) {
    const auto h = doc.handle(id);
    return h ? doc.get(*h) : nullptr;
}
bool isWorld(const document::Document& doc, NodeId id) {
    const auto* node = nodeOf(doc, id);
    return holds<document::Entity>(doc, id) && node->block && ciEquals(node->block->key(), "world");
}

// Rewrites the object's own `editor` block (and those of solids below it): `groupid`s and
// `visgroupid`s found in the maps are replaced, the others dropped. A null visgroup map keeps
// visgroups as they are. `fallback` becomes the group of an object left without one.
// Returns the object's resulting group.
std::optional<int> rewriteEditor(Kv& object, const IdMap& groups, const IdMap* vis,
                                 std::optional<int> fallback) {
    std::optional<int> result;
    Kv* editor = object.find("editor");
    if (!editor && fallback) {
        (void)object.append(Kv::block("editor"));
        editor = object.find("editor");
    }
    if (editor) {
        for (std::size_t i = editor->children().size(); i-- > 0;) {
            auto& child = editor->children()[i];
            if (child.kind() != kv1::NodeKind::Value) continue;
            const bool isGroup = ciEquals(child.key(), "groupid");
            const bool isVis = ciEquals(child.key(), "visgroupid");
            if (!isGroup && !(isVis && vis)) continue;
            const auto id = intOf(&child);
            const auto& map = isGroup ? groups : *vis;
            const auto it = id ? map.find(*id) : map.end();
            if (it == map.end()) {
                (void)editor->remove(i);
                continue;
            }
            if (isGroup) result = it->second;
            if (it->second != *id) (void)child.setValue(std::to_string(it->second));
        }
        if (!result && fallback) {
            (void)editor->append(Kv::value("groupid", std::to_string(*fallback)));
            result = fallback;
        }
    }
    for (auto& child : object.children()) {
        if (isBlock(child, "solid") || isBlock(child, "hidden")) rewriteEditor(child, groups, vis, std::nullopt);
    }
    return result;
}

void collectVisgroupIds(const Kv& object, std::set<int>& out) {
    if (const auto* editor = object.find("editor")) {
        for (const auto& child : editor->children()) {
            if (child.kind() == kv1::NodeKind::Value && ciEquals(child.key(), "visgroupid")) {
                if (const auto id = intOf(&child)) out.insert(*id);
            }
        }
    }
    for (const auto& child : object.children()) {
        if (isBlock(child, "solid") || isBlock(child, "hidden")) collectVisgroupIds(child, out);
    }
}

struct VisInfo {
    int id = 0;
    NodeId node = 0;
    std::string name, color;
    std::optional<int> parent;
};
void walkVisgroups(const document::Document& doc, NodeId id, std::optional<int> parent,
                   std::vector<VisInfo>& out) {
    const auto* node = nodeOf(doc, id);
    if (!node) return;
    std::optional<int> self = parent;
    if (holds<document::Visgroup>(doc, id) && node->block) {
        if (const auto vid = intOf(node->block->find("visgroupid"))) {
            out.push_back({*vid, id, stringOf(node->block.get(), "name"), stringOf(node->block.get(), "color"), parent});
            self = *vid;
        }
    }
    for (const auto child : node->children) walkVisgroups(doc, child, self, out);
}
std::vector<VisInfo> visgroupsOf(const document::Document& doc) {
    std::vector<VisInfo> out;
    for (const auto root : doc.roots()) {
        const auto* node = nodeOf(doc, root);
        if (node && node->block && holds<document::UnknownBlock>(doc, root) && ciEquals(node->block->key(), "visgroups"))
            walkVisgroups(doc, root, std::nullopt, out);
    }
    return out;
}

void forEachSolid(const Kv& block, const std::function<void(const Kv&)>& fn) {
    for (const auto& child : block.children()) {
        if (isBlock(child, "solid")) fn(child);
        else if (isBlock(child, "hidden")) forEachSolid(child, fn);
    }
}
void expandSolid(AABB& bounds, const Kv& solid) {
    const auto geometry = geometry::solidGeometry(solid);
    if (!geometry) return;
    for (const auto vertex : geometry->poly.vertices) bounds.expand(vertex);
}
void expandEntity(AABB& bounds, const Kv& entity) {
    bool solids = false;
    forEachSolid(entity, [&](const Kv& solid) {
        solids = true;
        expandSolid(bounds, solid);
    });
    if (solids) return;
    if (const auto* origin = entity.find("origin")) {
        if (const auto v = origin->asVec3()) bounds.expand(DVec3{(*v)[0], (*v)[1], (*v)[2]});
    }
}

struct FragVis {
    int id = 0;
    std::string name, color;
    std::optional<int> parent;
};
void flattenVisgroups(const Kv& block, std::optional<int> parent, std::vector<FragVis>& out) {
    for (const auto& child : block.children()) {
        if (!isBlock(child, "visgroup")) continue;
        const auto id = intOf(child.find("visgroupid"));
        auto name = stringOf(&child, "name");
        if (!id || name.empty()) {
            flattenVisgroups(child, parent, out);
            continue;
        }
        out.push_back({*id, std::move(name), stringOf(&child, "color"), parent});
        flattenVisgroups(child, *id, out);
    }
}

struct Fragment {
    std::vector<FragVis> visgroups;
    std::vector<Kv> groups, solids, entities;
};
Result<Fragment> parseFragment(std::string_view text) {
    auto parsed = kv1::parse(std::string(text));
    if (!parsed) return std::unexpected(parsed.error().withContext("clipboard text is not a VMF fragment"));
    Fragment fragment;
    for (const auto& child : parsed->root().children()) {
        if (child.kind() != kv1::NodeKind::Block) continue;
        if (ciEquals(child.key(), "visgroups")) flattenVisgroups(child, std::nullopt, fragment.visgroups);
        else if (ciEquals(child.key(), "group")) fragment.groups.push_back(child);
        else if (ciEquals(child.key(), "solid")) fragment.solids.push_back(child);
        else if (ciEquals(child.key(), "entity")) fragment.entities.push_back(child);
    }
    if (fragment.groups.empty() && fragment.solids.empty() && fragment.entities.empty())
        return std::unexpected(invalid("the clipboard holds no solids, entities or groups"));
    return fragment;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string expandAffix(const PasteSpecial& special, int copy) {
    std::string result;
    bool numbered = false;
    for (std::size_t i = 0; i < special.affix.size(); ++i) {
        if (special.affix.compare(i, 2, "%d") == 0) {
            result += std::to_string(copy);
            numbered = true;
            ++i;
        } else {
            result += special.affix[i];
        }
    }
    if (!numbered && special.copies > 1) {
        result += std::to_string(copy);
        if (special.names == NamePolicy::Prefix) result += '_';
    }
    return result;
}

bool referenceKey(std::string_view key) {
    for (const auto* skip : {"id", "classname", "targetname", "origin", "angles", "model", "spawnflags"}) {
        if (ciEquals(key, skip)) return false;
    }
    return true;
}
void rewriteReferences(Kv& entity, const std::map<std::string, std::string>& names) {
    for (auto& child : entity.children()) {
        if (child.kind() == kv1::NodeKind::Value && referenceKey(child.key())) {
            const auto value = child.decodedValue();
            if (!value) continue;
            if (const auto it = names.find(lower(*value)); it != names.end()) (void)child.setValue(it->second);
        } else if (isBlock(child, "connections")) {
            for (auto& entry : child.children()) {
                if (entry.kind() != kv1::NodeKind::Value) continue;
                const auto value = entry.decodedValue();
                if (!value) continue;
                auto connection = document::parseConnection(*value);
                if (!connection.wellFormed) continue;
                if (const auto it = names.find(lower(connection.target)); it != names.end()) {
                    connection.setField(document::ConnectionField::Target, it->second);
                    (void)entry.setValue(document::serialize(connection));
                }
            }
        }
    }
}

std::set<std::string> existingNames(const document::Document& doc) {
    std::set<std::string> names;
    for (const auto root : doc.roots()) {
        const auto* node = nodeOf(doc, root);
        if (!node || !node->block) continue;
        if (const auto name = stringOf(node->block.get(), "targetname"); !name.empty()) names.insert(lower(name));
    }
    return names;
}

// Finds or creates the visgroup `name` under `parent` (a root-level visgroup when empty).
Result<int> visgroupByName(const document::Document& doc, document::Transaction& tx, const FragVis& wanted,
                           std::optional<int> parent) {
    const auto infos = visgroupsOf(doc);
    for (const auto& info : infos) {
        if (info.parent == parent && ciEquals(info.name, wanted.name)) return info.id;
    }
    std::optional<document::NodeHandle> container;
    if (parent) {
        for (const auto& info : infos) {
            if (info.id == *parent) container = doc.handle(info.node);
        }
    } else {
        for (const auto root : doc.roots()) {
            const auto* node = nodeOf(doc, root);
            if (node && node->block && holds<document::UnknownBlock>(doc, root) &&
                ciEquals(node->block->key(), "visgroups"))
                container = doc.handle(root);
        }
        if (!container) {
            container = tx.create(Kv::block("visgroups"));
            std::size_t position = 0;
            for (std::size_t i = 0; i < doc.roots().size(); ++i) {
                const auto* node = nodeOf(doc, doc.roots()[i]);
                if (node && node->block && ciEquals(node->block->key(), "versioninfo")) position = i + 1;
            }
            tx.reparent(*container, std::nullopt, position);
        }
    }
    if (!container) return std::unexpected(invalid("visgroup parent is missing"));
    auto block = Kv::block("visgroup");
    (void)block.append(Kv::value("name", wanted.name));
    (void)block.append(Kv::value("visgroupid", "0"));
    if (!wanted.color.empty()) (void)block.append(Kv::value("color", wanted.color));
    const auto handle = tx.create(std::move(block), container);
    const auto id = intOf(doc.get(handle)->block->find("visgroupid"));
    if (!id) return std::unexpected(Error{ErrorCode::Internal, "new visgroup has no id"});
    return *id;
}

} // namespace

Result<std::string> serializeSelection(const document::Document& doc, const GroupIndex& groups,
                                       const Selection& selection) {
    std::set<NodeId> chosen;
    for (const auto id : selectedNodes(selection)) {
        if (!doc.handle(id) || isWorld(doc, id)) continue;
        if (holds<document::Group>(doc, id)) {
            chosen.insert(id);
            for (const auto member : groups.members(id, true)) chosen.insert(member);
        } else if (holds<document::Solid>(doc, id) || holds<document::Entity>(doc, id)) {
            chosen.insert(id);
        }
    }
    if (chosen.empty()) return std::unexpected(invalid("nothing selected to copy"));
    // A brush entity copies as itself when all of its solids are chosen.
    std::set<NodeId> owners;
    for (const auto id : chosen) {
        if (holds<document::Solid>(doc, id)) {
            if (const auto owner = brushEntityOf(doc, id)) owners.insert(*owner);
        }
    }
    for (const auto owner : owners) {
        const auto solids = solidsOf(doc, owner);
        if (std::ranges::all_of(solids, [&](NodeId s) { return chosen.contains(s); })) chosen.insert(owner);
    }
    std::erase_if(chosen, [&](NodeId id) {
        if (!holds<document::Solid>(doc, id)) return false;
        const auto owner = brushEntityOf(doc, id);
        return owner && chosen.contains(*owner);
    });

    std::vector<Kv> groupBlocks, solidBlocks, entityBlocks;
    IdMap identity;
    for (const auto id : chosen) {
        if (holds<document::Group>(doc, id)) {
            if (const auto vid = vmfId(doc, id)) identity[*vid] = *vid;
        }
    }
    const auto visit = [&](const auto& self, NodeId id) -> void {
        const auto* node = nodeOf(doc, id);
        if (!node) return;
        if (chosen.contains(id)) {
            auto block = doc.exportSubtree(id);
            rewriteEditor(block, identity, nullptr, std::nullopt);
            if (holds<document::Group>(doc, id)) groupBlocks.push_back(std::move(block));
            else if (holds<document::Solid>(doc, id)) solidBlocks.push_back(std::move(block));
            else entityBlocks.push_back(std::move(block));
            return;
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) visit(visit, root);

    std::set<int> used;
    for (const auto* blocks : {&groupBlocks, &solidBlocks, &entityBlocks}) {
        for (const auto& block : *blocks) collectVisgroupIds(block, used);
    }
    const auto infos = visgroupsOf(doc);
    std::set<int> keep;
    for (const auto& info : infos) {
        if (!used.contains(info.id)) continue;
        for (auto at = std::optional<int>(info.id); at;) {
            if (!keep.insert(*at).second) break;
            const auto it = std::ranges::find(infos, *at, &VisInfo::id);
            at = it == infos.end() ? std::nullopt : it->parent;
        }
    }
    kv1::Document out;
    if (!keep.empty()) {
        const auto build = [&](const auto& self, std::optional<int> parent) -> std::vector<Kv> {
            std::vector<Kv> result;
            for (const auto& info : infos) {
                if (info.parent != parent || !keep.contains(info.id)) continue;
                auto block = Kv::block("visgroup");
                (void)block.append(Kv::value("name", info.name));
                (void)block.append(Kv::value("visgroupid", std::to_string(info.id)));
                if (!info.color.empty()) (void)block.append(Kv::value("color", info.color));
                for (auto& child : self(self, info.id)) (void)block.append(std::move(child));
                result.push_back(std::move(block));
            }
            return result;
        };
        auto container = Kv::block("visgroups");
        for (auto& block : build(build, std::nullopt)) (void)container.append(std::move(block));
        (void)out.root().append(std::move(container));
    }
    for (auto* blocks : {&groupBlocks, &solidBlocks, &entityBlocks}) {
        for (auto& block : *blocks) {
            if (auto r = out.root().append(std::move(block)); !r) return std::unexpected(r.error());
        }
    }
    return kv1::write(out);
}


static void collectSideIds(const Kv& block, std::vector<int>& out) {
    for (const auto& child : block.children()) {
        if (child.kind() != kv1::NodeKind::Block) continue;
        if (ciEquals(child.key(), "side")) {
            if (const auto id = intOf(child.find("id"))) out.push_back(*id);
        } else if (ciEquals(child.key(), "solid")) {
            collectSideIds(child, out);
        }
    }
}
// Pair the source block's sides with the created ones; the document re-ids them in order.
static void recordSideIds(const Kv& source, const Kv& created, std::map<int, int>& out) {
    std::vector<int> from, to;
    collectSideIds(source, from);
    collectSideIds(created, to);
    for (std::size_t i = 0; i < from.size() && i < to.size(); ++i) out[from[i]] = to[i];
}

Result<EditedObjects> stagePaste(const document::Document& doc, document::Transaction& tx,
                                 std::string_view text, const PasteOptions& options) {
    using Kind = EditedObjects::Item::Kind;
    auto parsed = parseFragment(text);
    if (!parsed) return std::unexpected(parsed.error());
    auto& fragment = *parsed;
    const PasteSpecial special = options.special.value_or(PasteSpecial{});
    if (special.copies < 1 || special.copies > PasteSpecial::kMaxCopies)
        return std::unexpected(invalid("copies must be between 1 and " + std::to_string(PasteSpecial::kMaxCopies)));
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(special.offset[axis]) || !std::isfinite(special.rotationDeg[axis]))
            return std::unexpected(invalid("paste special offset and rotation must be finite"));
    }

    // Visgroups by name, parents first (a fragment lists them depth first).
    IdMap visMap;
    for (const auto& wanted : fragment.visgroups) {
        std::optional<int> parent;
        if (wanted.parent) {
            const auto it = visMap.find(*wanted.parent);
            if (it == visMap.end()) continue;
            parent = it->second;
        }
        auto id = visgroupByName(doc, tx, wanted, parent);
        if (!id) return std::unexpected(id.error());
        visMap[wanted.id] = *id;
    }

    AABB bounds;
    for (const auto& solid : fragment.solids) expandSolid(bounds, solid);
    for (const auto& entity : fragment.entities) expandEntity(bounds, entity);

    Transform placement;
    if (!options.inPlace && !bounds.empty()) {
        const SurfacePlane plane = options.plane.value_or(SurfacePlane{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {}});
        const DVec3 size = bounds.max - bounds.min;
        const double half = (std::abs(plane.normal.x) * size.x + std::abs(plane.normal.y) * size.y +
                             std::abs(plane.normal.z) * size.z) * 0.5;
        const DVec3 grab = bounds.center() - plane.normal * half;
        const DVec3 raw = options.at.value_or(plane.origin) - grab;
        placement = Transform::translate(snapTranslate(raw, bounds, grab, plane, options.snap.value_or(SnapSettings{})));
    }

    // Groups, parents before children. Unresolvable or cyclic parents are cut loose.
    std::vector<std::size_t> order;
    {
        std::map<int, std::size_t> bySource;
        for (std::size_t i = 0; i < fragment.groups.size(); ++i) {
            if (const auto id = intOf(fragment.groups[i].find("id"))) bySource.emplace(*id, i);
        }
        const auto parentOf = [&](std::size_t i) -> std::optional<std::size_t> {
            const auto* editor = fragment.groups[i].find("editor");
            const auto parent = intOf(editor ? editor->find("groupid") : nullptr);
            if (!parent) return std::nullopt;
            const auto it = bySource.find(*parent);
            if (it == bySource.end() || it->second == i) return std::nullopt;
            return it->second;
        };
        std::set<std::size_t> placed;
        while (placed.size() < fragment.groups.size()) {
            bool progress = false;
            for (std::size_t i = 0; i < fragment.groups.size(); ++i) {
                if (placed.contains(i)) continue;
                const auto parent = parentOf(i);
                if (parent && !placed.contains(*parent)) continue;
                placed.insert(i);
                order.push_back(i);
                progress = true;
            }
            if (!progress) { // A cycle: emit the rest; their unplaced parents resolve to no group.
                for (std::size_t i = 0; i < fragment.groups.size(); ++i) {
                    if (placed.insert(i).second) order.push_back(i);
                }
            }
        }
    }

    auto used = existingNames(doc);
    const auto world = worldHandle(doc);
    const DVec3 centre = bounds.empty() ? DVec3{0.0} : bounds.center();
    EditedObjects result;
    Transform previous = placement;
    for (int copy = 1; copy <= special.copies; ++copy) {
        Transform current = placement;
        if (options.special) {
            const DVec3 c = previous.point(centre);
            Transform rotation;
            const DVec3 r = special.rotationDeg;
            if (r.x != 0.0) rotation = Transform::rotate({1, 0, 0}, r.x, c) * rotation;
            if (r.y != 0.0) rotation = Transform::rotate({0, 1, 0}, r.y, c) * rotation;
            if (r.z != 0.0) rotation = Transform::rotate({0, 0, 1}, r.z, c) * rotation;
            current = Transform::translate(special.offset) * rotation * previous;
            previous = current;
        }

        std::optional<int> wrapperId;
        std::optional<NodeId> wrapperNode;
        if (special.groupCopies) {
            auto block = Kv::block("group");
            (void)block.append(Kv::value("id", "0"));
            const auto handle = tx.create(std::move(block), world);
            wrapperNode = doc.get(handle)->id;
            wrapperId = vmfId(doc, *wrapperNode);
            if (wrapperId) tx.setEditor(handle, document::EditorEdit::setColor(groupColor(*wrapperId)));
            result.groupParents[*wrapperNode] = std::nullopt;
        }

        IdMap groupMap;
        std::map<int, NodeId> groupNodes;
        for (const auto index : order) {
            auto block = fragment.groups[index];
            const auto source = intOf(block.find("id"));
            const auto parent = rewriteEditor(block, groupMap, &visMap, wrapperId);
            const auto handle = tx.create(std::move(block), world);
            const auto node = doc.get(handle)->id;
            const auto created = vmfId(doc, node);
            if (!created) return std::unexpected(Error{ErrorCode::Internal, "new group has no id"});
            if (source) groupMap[*source] = *created;
            groupNodes[*created] = node;
            std::optional<NodeId> parentNode = wrapperNode;
            if (parent && groupNodes.contains(*parent)) parentNode = groupNodes.at(*parent);
            else if (parent && parent != wrapperId) parentNode = std::nullopt;
            result.groupParents[node] = parentNode;
        }

        std::map<std::string, std::string> renamed; // lower(old) -> new
        if (special.names != NamePolicy::None) {
            const auto affix = expandAffix(special, copy);
            for (const auto& entity : fragment.entities) {
                const auto name = stringOf(&entity, "targetname");
                if (name.empty() || renamed.contains(lower(name))) continue;
                const auto base = special.names == NamePolicy::Prefix ? affix + name : name + affix;
                auto candidate = base;
                for (int n = 2; used.contains(lower(candidate)); ++n) candidate = base + "_" + std::to_string(n);
                used.insert(lower(candidate));
                renamed[lower(name)] = std::move(candidate);
            }
        }

        std::vector<NodeId> moved;
        const auto groupNodeOf = [&](std::optional<int> id) -> std::optional<NodeId> {
            if (!id) return std::nullopt;
            if (const auto it = groupNodes.find(*id); it != groupNodes.end()) return it->second;
            return wrapperNode;
        };
        for (const auto& source : fragment.solids) {
            auto block = source;
            const auto group = rewriteEditor(block, groupMap, &visMap, wrapperId);
            const auto handle = tx.create(std::move(block), world);
            const auto node = doc.get(handle)->id;
            recordSideIds(source, doc.exportSubtree(node), result.sideIds);
            moved.push_back(node);
            EditedObjects::Item item;
            item.node = node;
            item.kind = Kind::Solid;
            item.solids = {node};
            item.group = groupNodeOf(group);
            result.items.push_back(std::move(item));
        }
        for (const auto& source : fragment.entities) {
            auto block = source;
            const auto group = rewriteEditor(block, groupMap, &visMap, wrapperId);
            if (!renamed.empty()) {
                if (special.rewriteReferences) rewriteReferences(block, renamed);
                if (const auto it = renamed.find(lower(stringOf(&block, "targetname"))); it != renamed.end()) {
                    if (auto* name = block.find("targetname")) (void)name->setValue(it->second);
                }
            }
            const auto handle = tx.create(std::move(block));
            const auto node = doc.get(handle)->id;
            recordSideIds(source, doc.exportSubtree(node), result.sideIds);
            moved.push_back(node);
            EditedObjects::Item item;
            item.node = node;
            item.kind = Kind::Entity;
            item.solids = solidsOf(doc, node);
            item.group = groupNodeOf(group);
            result.items.push_back(std::move(item));
        }
        if (current.matrix() != DMat4{1.0} && !moved.empty()) {
            if (auto done = stageTransform(doc, tx, moved, current, {options.textureLock.value_or(true), options.classes}); !done)
                return std::unexpected(done.error());
        }
    }
    return result;
}

} // namespace apex::editor
