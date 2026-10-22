#pragma once

#include "editor/hide_ops.hpp"

#include <set>
#include <string_view>

namespace apex::editor {

struct VisgroupNode {
    NodeId node = 0;
    int vmfId = 0;
    std::string name, color;
    NodeId parent = 0;
    std::vector<NodeId> children;
};
struct VisgroupTree {
    std::vector<VisgroupNode> nodes;
    std::map<int, std::vector<NodeId>> membership;
    std::map<NodeId, std::vector<int>> memberships;
    std::set<int> hidden;
    std::set<int> dangling;
    const VisgroupNode* find(int id) const;
    std::vector<NodeId> members(int id) const;
    std::vector<int> visgroupsOf(NodeId node) const;
    bool shown(int id) const { return !hidden.contains(id); }
};
VisgroupTree buildVisgroupTree(const document::Document& doc);

struct AutoVisgroup {
    std::string_view id, name;
    bool (*matches)(const document::Document&, NodeId);
};
std::span<const AutoVisgroup> autoVisgroups();
std::vector<NodeId> autoMembers(const document::Document& doc, std::string_view id);

// VMF records one flag per object, not the individual overlapping hide reasons. Sessions
// retain these reasons with undo history. Loading conservatively infers them from flags.
struct VisgroupVisibility {
    std::set<int> user;
    std::set<std::string, std::less<>> automatic;
    std::set<NodeId> wrappers;
};
VisgroupVisibility buildVisgroupVisibility(const document::Document& doc);
Result<int> stageCreateVisgroup(const document::Document& doc, std::string name,
    std::optional<int> parent, document::Transaction& tx);
Result<void> stageRenameVisgroup(const document::Document& doc, int id, std::string name,
    document::Transaction& tx);
Result<void> stageMoveVisgroup(const document::Document& doc, int id, std::optional<int> parent,
    std::size_t position, document::Transaction& tx);
Result<void> stageDeleteVisgroup(const document::Document& doc, int id,
    VisgroupVisibility& visibility, document::Transaction& tx);
Result<void> stageVisgroupMembership(const document::Document& doc, const GroupIndex& groups,
    const Selection& selection, int id, bool add, document::Transaction& tx);
Result<void> stageSetVisgroupShown(const document::Document& doc, int id, bool shown,
    VisgroupVisibility& visibility, document::Transaction& tx);
Result<void> stageSetAutoVisgroupShown(const document::Document& doc, std::string_view id,
    bool shown, VisgroupVisibility& visibility, document::Transaction& tx);

} // namespace apex::editor
