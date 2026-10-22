#pragma once

#include "document/document.hpp"

#include <map>
#include <set>

namespace apex::editor {

using document::NodeId;

// Membership comes only from editor.groupid, never document ancestry. VMF IDs and
// NodeIds are separate spaces. Missing references are ignored; duplicate group IDs
// choose the first NodeId. Cycle edges between groups are ignored as broken links.
// The document must outlive this index, including its optional synchronous observer.
class GroupIndex {
public:
    explicit GroupIndex(const document::Document& document);
    ~GroupIndex();
    GroupIndex(const GroupIndex&) = delete;
    GroupIndex& operator=(const GroupIndex&) = delete;
    GroupIndex(GroupIndex&&) = delete;
    GroupIndex& operator=(GroupIndex&&) = delete;

    void rebuild();
    void apply(const document::ChangeSet& changes);
    void attach(document::Document& document);
    void detach();
    const document::Document& document() const { return doc_; }
    std::optional<NodeId> group(int vmfId) const;
    std::optional<NodeId> parentGroup(NodeId object) const;
    // Includes the object itself when it is an unparented group.
    std::optional<NodeId> outermostGroup(NodeId object) const;
    // Sorted, unique NodeIds, including nested group nodes when recursive is true.
    std::vector<NodeId> members(NodeId group, bool recursive = false) const;
    std::size_t size() const { return groups_.size(); }
    std::size_t membershipCount() const { return parents_.size(); }

private:
    struct Record {
        NodeId parent;
        std::optional<int> groupId, membership;
        bool editor;
    };
    const document::Document& doc_;
    std::map<NodeId, Record> records_;
    std::map<document::NodeHandle, NodeId> handles_;
    std::map<int, NodeId> groups_;
    std::map<int, std::set<NodeId>> definitions_, references_;
    std::map<NodeId, NodeId> parents_;
    std::map<NodeId, std::set<NodeId>> members_;
    document::Document* observed_ = nullptr;
    std::size_t observer_ = 0;

    void refresh(NodeId id);
    void link(std::set<NodeId> affected, const std::set<int>& changedGroups);
};

} // namespace apex::editor
