#pragma once

#include "document/document.hpp"

#include <set>
#include <span>

namespace apex::editor {

class PickIndex;
class GroupIndex;
using document::NodeId;
using document::SideId;
using Target = std::variant<NodeId, SideId>;
enum class SelectionMode { Groups, Objects, Solids, Faces };

// Ordered unique targets; mode determines the variant alternative. Groups store a
// group NodeId, not expanded members. Changing mode clears the old selection.
// Selection is session state: none of these calls write to Document or its history.
// Copies are snapshots without observers; assignment preserves destination observers.
class Selection {
public:
    using Mode = SelectionMode;
    explicit Selection(Mode mode = Mode::Groups) : mode_(mode) {}
    Selection(const Selection& other);
    Selection& operator=(const Selection& other);

    Mode mode() const { return mode_; }
    void setMode(Mode mode);
    bool ignoreGroups() const { return ignoreGroups_; }
    void setIgnoreGroups(bool ignore);
    void select(Target target);
    void add(Target target);
    void toggle(Target target);
    void remove(Target target);
    void clear();
    void selectNone() { clear(); }
    bool contains(Target target) const;
    const std::vector<Target>& ordered() const { return ordered_; }
    std::optional<Target> active() const { return active_; }
    // Bulk update, preserving first occurrence order. Active defaults to the last target.
    void replace(std::span<const Target> targets);
    void selectAll(const document::Document& doc, const PickIndex& picks);
    void invert(const document::Document& doc, const PickIndex& picks);
    void prune(const document::Document& doc);

    // v1 words: magic, version, mode, ignoreGroups, active index+1 (0 = none),
    // count, then IDs in selection order. Faces use nonnegative SideIds. Invalid
    // or unsupported blobs return false and leave the selection untouched.
    document::SelectionBlob toBlob() const;
    bool fromBlob(const document::SelectionBlob& blob);
    using Observer = std::function<void(const Selection&)>;
    std::size_t observe(Observer observer);
    void unobserve(std::size_t token);
    std::uint64_t changed() const { return changed_; }

private:
    Mode mode_;
    bool ignoreGroups_ = false;
    std::vector<Target> ordered_;
    std::optional<Target> active_;
    std::set<Target> members_;
    std::map<std::size_t, Observer> observers_;
    std::size_t nextObserver_ = 1;
    std::uint64_t changed_ = 0;
    void notify();
    void validate(Target target) const;
};

} // namespace apex::editor
