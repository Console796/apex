#include "editor/group_index.hpp"

#include "core/strings.hpp"

#include <stdexcept>

namespace apex::editor {
namespace {

std::optional<int> number(const kv1::Node& block, std::string_view key) {
    if (const auto* value = block.find(key)) {
        if (const auto n = value->asInt()) return *n;
    }
    return std::nullopt;
}

} // namespace

GroupIndex::GroupIndex(const document::Document& doc) : doc_(doc) { rebuild(); }
GroupIndex::~GroupIndex() { detach(); }

void GroupIndex::attach(document::Document& doc) {
    if (&doc != &doc_) throw std::invalid_argument("GroupIndex observer document differs");
    if (observed_) return;
    rebuild();
    observer_ = doc.observe([this](const document::ChangeSet& changes) { apply(changes); });
    observed_ = &doc;
}

void GroupIndex::detach() {
    if (observed_) observed_->unobserve(observer_);
    observed_ = nullptr;
}

void GroupIndex::refresh(NodeId id) {
    const auto h = doc_.handle(id);
    const auto* node = h ? doc_.get(*h) : nullptr;
    if (const auto old = records_.find(id); old != records_.end()) {
        const auto erase = [&](auto& index, std::optional<int> key) {
            if (!key) return;
            auto& values = index.at(*key);
            values.erase(id);
            if (values.empty()) index.erase(*key);
        };
        erase(definitions_, old->second.groupId);
        erase(references_, old->second.membership);
        records_.erase(old);
    }
    if (!node || !node->block) return;
    handles_[*h] = id;
    const bool isGroup = std::holds_alternative<Handle<document::Group>>(*h);
    const bool object = isGroup || std::holds_alternative<Handle<document::Entity>>(*h) ||
                        std::holds_alternative<Handle<document::Solid>>(*h);
    Record record{node->parent, isGroup ? number(*node->block, "id") : std::nullopt,
                  std::nullopt, ciEquals(node->block->key(), "editor")};
    if (object) {
        const kv1::Node* editor = nullptr;
        for (const auto& child : node->block->children()) {
            if (child.kind() == kv1::NodeKind::Block && ciEquals(child.key(), "editor")) {
                editor = &child;
                break;
            }
        }
        if (!editor) {
            for (const auto child : node->children) {
                const auto ch = doc_.handle(child);
                const auto* n = ch ? doc_.get(*ch) : nullptr;
                if (n && n->block && ciEquals(n->block->key(), "editor")) {
                    editor = n->block.get();
                    break;
                }
            }
        }
        if (editor) record.membership = number(*editor, "groupid");
    }
    records_.emplace(id, record);
    if (record.groupId) definitions_[*record.groupId].insert(id);
    if (record.membership) references_[*record.membership].insert(id);
}

void GroupIndex::link(std::set<NodeId> affected, const std::set<int>& changedGroups) {
    for (const auto vmfId : changedGroups) {
        groups_.erase(vmfId);
        if (const auto it = definitions_.find(vmfId); it != definitions_.end()) {
            groups_[vmfId] = *it->second.begin();
        }
        // A definition can resolve dangling members or redirect duplicate IDs.
        if (const auto it = references_.find(vmfId); it != references_.end()) {
            affected.insert(it->second.begin(), it->second.end());
        }
    }
    // Reconsider group edges so repairing a malformed cycle revives valid links.
    // Ordinary member edits never rescan unrelated member records.
    for (const auto& [vmfId, ids] : definitions_) {
        (void)vmfId;
        affected.insert(ids.begin(), ids.end());
    }
    const auto unlink = [&](NodeId id) {
        if (const auto it = parents_.find(id); it != parents_.end()) {
            auto& members = members_.at(it->second);
            members.erase(id);
            if (members.empty()) members_.erase(it->second);
            parents_.erase(it);
        }
    };
    for (const auto id : affected) {
        unlink(id);
        const auto it = records_.find(id);
        if (it == records_.end() || !it->second.membership) continue;
        if (const auto parent = group(*it->second.membership)) {
            parents_[id] = *parent;
            members_[*parent].insert(id);
        }
    }
    std::set<NodeId> broken;
    for (const auto& [vmfId, id] : groups_) {
        (void)vmfId;
        std::set<NodeId> path;
        auto current = id;
        while (parents_.contains(current)) {
            if (!path.insert(current).second) {
                const auto first = current;
                do {
                    broken.insert(current);
                    current = parents_.at(current);
                } while (current != first);
                break;
            }
            current = parents_.at(current);
        }
    }
    for (const auto id : broken) unlink(id);
}

void GroupIndex::rebuild() {
    records_.clear();
    handles_.clear();
    definitions_.clear();
    references_.clear();
    groups_.clear();
    parents_.clear();
    members_.clear();
    std::set<NodeId> affected;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        refresh(id);
        affected.insert(id);
        const auto h = doc_.handle(id);
        if (const auto* n = h ? doc_.get(*h) : nullptr) {
            for (const auto child : n->children) self(self, child);
        }
    };
    for (const auto root : doc_.roots()) walk(walk, root);
    std::set<int> changedGroups;
    for (const auto& [vmfId, ids] : definitions_) {
        (void)ids;
        changedGroups.insert(vmfId);
    }
    link(std::move(affected), changedGroups);
}

void GroupIndex::apply(const document::ChangeSet& changes) {
    if (changes.empty()) return;
    std::set<NodeId> affected;
    const auto touch = [&](document::NodeHandle h) {
        if (const auto old = handles_.find(h); old != handles_.end()) {
            affected.insert(old->second);
            const auto& record = records_.at(old->second);
            if (record.editor) affected.insert(record.parent);
        }
        if (const auto* n = doc_.get(h)) {
            affected.insert(n->id);
            if (n->block && ciEquals(n->block->key(), "editor")) affected.insert(n->parent);
        }
    };
    for (const auto h : changes.created) touch(h);
    for (const auto& m : changes.modified) {
        if ((static_cast<unsigned>(m.flags) & static_cast<unsigned>(
                document::Dirty::Hierarchy | document::Dirty::KeyValues)) != 0) touch(m.handle);
    }
    for (const auto h : changes.destroyed) touch(h);
    for (const auto h : changes.destroyed) handles_.erase(h);
    std::set<int> changedGroups;
    for (const auto id : affected) {
        if (const auto it = records_.find(id); it != records_.end() && it->second.groupId) {
            changedGroups.insert(*it->second.groupId);
        }
        refresh(id);
        if (const auto it = records_.find(id); it != records_.end() && it->second.groupId) {
            changedGroups.insert(*it->second.groupId);
        }
    }
    if (!affected.empty()) link(std::move(affected), changedGroups);
}

std::optional<NodeId> GroupIndex::group(int vmfId) const {
    const auto it = groups_.find(vmfId);
    return it == groups_.end() ? std::nullopt : std::optional(it->second);
}

std::optional<NodeId> GroupIndex::parentGroup(NodeId object) const {
    const auto it = parents_.find(object);
    return it == parents_.end() ? std::nullopt : std::optional(it->second);
}

std::optional<NodeId> GroupIndex::outermostGroup(NodeId object) const {
    std::optional<NodeId> result;
    const auto it = records_.find(object);
    if (it != records_.end() && it->second.groupId) result = object;
    while (const auto parent = parentGroup(object)) {
        result = parent;
        object = *parent;
    }
    return result;
}

std::vector<NodeId> GroupIndex::members(NodeId groupId, bool recursive) const {
    std::set<NodeId> result;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        if (const auto it = members_.find(id); it != members_.end()) {
            for (const auto member : it->second) {
                if (result.insert(member).second && recursive) self(self, member);
            }
        }
    };
    walk(walk, groupId);
    return {result.begin(), result.end()};
}

} // namespace apex::editor
