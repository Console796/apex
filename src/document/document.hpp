#pragma once

#include "core/handle.hpp"
#include "document/vmf.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace apex::document {

using NodeId = std::uint64_t;
using SideId = int;
enum class Dirty : unsigned {
    None = 0, Geometry = 1, Material = 2, KeyValues = 4, Visibility = 8, Hierarchy = 16
};
constexpr Dirty operator|(Dirty a, Dirty b) {
    return static_cast<Dirty>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

// Plain node values. Hierarchy IDs are document-local, stable across undo, and resolved with
// handle(). Handles themselves never revive after deletion (including undo of a creation).
// The immutable codec block preserves unknown keys, duplicates, spans and original spelling.
struct Node {
    NodeId id = 0;
    NodeId parent = 0;
    std::vector<NodeId> children;
    std::vector<std::pair<std::size_t, NodeId>> sourceChildren;
    std::shared_ptr<const kv1::Node> block;
    Dirty dirty = Dirty::None;
};
struct Entity : Node {};
struct Solid : Node {}; // Side blocks live by value inside block, in source face order (D6/D21).
struct Group : Node {};
struct Visgroup : Node {};
struct Camera : Node {};
struct Cordon : Node {};
struct UnknownBlock : Node {}; // Also retains structural containers such as hidden/visgroups.
using NodeHandle = std::variant<Handle<Entity>, Handle<Solid>, Handle<Group>, Handle<Visgroup>,
                                Handle<Camera>, Handle<Cordon>, Handle<UnknownBlock>>;
using NodeValue = std::variant<Entity, Solid, Group, Visgroup, Camera, Cordon, UnknownBlock>;

struct Modified {
    NodeHandle handle;
    Dirty flags = Dirty::None;
};
struct SideLocation {
    Handle<Solid> solid;
    std::size_t faceIndex = 0;
    bool operator==(const SideLocation&) const = default;
};
struct ChangeSet {
    std::vector<NodeHandle> created;
    std::vector<Modified> modified;
    std::vector<NodeHandle> destroyed;
    std::vector<SideId> vanishedSides;
    bool empty() const {
        return created.empty() && modified.empty() && destroyed.empty() && vanishedSides.empty();
    }
};

class Transaction;
// Single writer thread, including synchronous observers; no locking. Only one transaction
// may be active. Observers may read the completed state but must not mutate the document.
// Node pointers are invalidated by arena mutations. Resolve handles again after mutation.
class Document {
public:
    explicit Document(const vmf::VmfFile& file, std::size_t undoCap = 512ULL * 1024 * 1024);
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    Transaction begin(std::string label);
    const Node* get(NodeHandle handle) const;
    template <typename T> const T* get(Handle<T> h) const { return pool<T>().get(h); }
    std::optional<NodeHandle> handle(NodeId id) const;
    const std::vector<NodeId>& roots() const { return roots_; }
    std::optional<SideLocation> side(SideId id) const;
    // Duplicate side IDs are preserved on load; side() returns the first hierarchy occurrence.
    std::vector<NodeHandle> dirtyNodes() const;
    Result<vmf::VmfFile> toVmf() const;
    Result<std::string> write() const;

    bool undo();
    bool redo();
    std::size_t undoCount() const { return undo_.size(); }
    std::size_t redoCount() const { return redo_.size(); }
    std::size_t historyBytes() const { return historyBytes_; }
    void setUndoCap(std::size_t bytes);
    using Observer = std::function<void(const ChangeSet&)>;
    std::size_t observe(Observer observer);
    void unobserve(std::size_t token);

private:
    friend class Transaction;
    using Pools = std::tuple<Pool<Entity>, Pool<Solid>, Pool<Group>, Pool<Visgroup>,
                             Pool<Camera>, Pool<Cordon>, Pool<UnknownBlock>>;
    template <typename T> const Pool<T>& pool() const { return std::get<Pool<T>>(pools_); }
    struct Delta {
        NodeId id;
        std::optional<NodeValue> before, after;
        Dirty flags = Dirty::None;
        std::optional<NodeHandle> beforeHandle;
    };
    struct Entry {
        std::string label;
        std::vector<Delta> deltas;
        std::vector<NodeId> rootsBefore, rootsAfter;
        std::optional<std::pair<NodeId, std::string>> coalesce;
        std::size_t bytes = 0;
    };
    Pools pools_;
    kv1::Document source_;
    std::map<NodeId, NodeHandle> handles_;
    std::vector<NodeId> roots_, originalRoots_;
    std::map<SideId, SideLocation> sides_;
    std::int64_t nextObject_ = 1, nextSide_ = 1, nextGroup_ = 1, nextVisgroup_ = 1;
    NodeId nextNode_ = 1;
    std::deque<Entry> undo_, redo_;
    std::size_t cap_, historyBytes_ = 0, nextObserver_ = 1;
    std::map<std::size_t, Observer> observers_;
    bool active_ = false, notifying_ = false, coalesceAllowed_ = false;

    NodeId import(const kv1::Node& block, NodeId parent);
    NodeValue value(NodeId id) const;
    Node* node(NodeId id);
    void put(NodeId id, const std::optional<NodeValue>& value);
    kv1::Node exportNode(NodeId id) const;
    void exportChildren(kv1::Node& block, const std::vector<NodeId>& children,
                        const std::vector<std::pair<std::size_t, NodeId>>& slots) const;
    void indexSides();
    void notify(const ChangeSet& changes);
    ChangeSet restore(const Entry& entry, bool after);
    void evict();
    static std::size_t memory(const Entry& entry);
    static int allocate(std::int64_t& next);
};

// Writes take effect immediately, with one notification on commit. Destruction without
// commit rolls back. ID allocators are monotonic even across rollback/undo. Invalid handles,
// cycles, identity-key edits and nested transactions throw std::invalid_argument/logic_error.
class Transaction {
public:
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&& other) noexcept;
    ~Transaction();

    // Creates a VMF block (entity/solid/group/visgroup/camera/cordon, or opaque container).
    // Fresh IDs replace IDs in created entity/solid/group/visgroup blocks and solid sides.
    NodeHandle create(kv1::Node block, std::optional<NodeHandle> parent = {});
    void destroy(NodeHandle handle); // Recursive, preserving sibling order on undo.
    void reparent(NodeHandle handle, std::optional<NodeHandle> parent,
                  std::size_t position = static_cast<std::size_t>(-1));
    void setKeyValue(NodeHandle handle, std::string key, std::optional<std::string> value);
    // Side identity is stable; the face index is resolved internally for each write.
    void setSideKeyValue(SideId side, std::string key, std::string value);
    ChangeSet commit(); // Consecutive single edits of the same node/key coalesce.

private:
    friend class Document;
    explicit Transaction(Document& document, std::string label);
    Document* doc_;
    Document::Entry entry_;
    std::optional<std::pair<NodeId, std::string>> key_;
    std::size_t operations_ = 0;
    NodeId checked(NodeHandle handle) const;
    void touch(NodeId id, Dirty flags);
    void detach(NodeId id);
    void destroyId(NodeId id);
};

} // namespace apex::document
