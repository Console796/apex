#include "document/document.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace apex::document {
namespace {

Node& base(NodeValue& value) {
    return std::visit([](auto& node) -> Node& { return node; }, value);
}
const Node& base(const NodeValue& value) {
    return std::visit([](const auto& node) -> const Node& { return node; }, value);
}
NodeValue typed(std::string_view name) {
    if (ciEquals(name, "world") || ciEquals(name, "entity")) return Entity{};
    if (ciEquals(name, "solid")) return Solid{};
    if (ciEquals(name, "group")) return Group{};
    if (ciEquals(name, "visgroup")) return Visgroup{};
    if (ciEquals(name, "camera")) return Camera{};
    if (ciEquals(name, "cordon")) return Cordon{};
    return UnknownBlock{};
}
void set(kv1::Node& block, const std::string& key, const std::optional<std::string>& value) {
    for (std::size_t i = 0; i < block.children().size(); ++i) {
        auto& child = block.children()[i];
        if (child.kind() == kv1::NodeKind::Value && ciEquals(child.key(), key)) {
            if (value) {
                if (child.decodedValue() != *value) child.setValue(*value).value();
            } else {
                block.remove(i).value();
            }
            return;
        }
    }
    if (value) block.append(kv1::Node::value(key, *value)).value();
}
std::string folded(std::string key) {
    for (auto& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return key;
}
bool sameUv(const geometry::FaceUv& a, const geometry::FaceUv& b) {
    return a.uaxis == b.uaxis && a.vaxis == b.vaxis && a.shiftU == b.shiftU &&
           a.shiftV == b.shiftV && a.scaleU == b.scaleU && a.scaleV == b.scaleV;
}
bool samePolygon(const geometry::ConvexPolyhedron& a, const geometry::Face& fa,
                 const geometry::ConvexPolyhedron& b, const geometry::Face& fb) {
    if (fa.vertices.size() != fb.vertices.size()) return false;
    const auto count = fa.vertices.size();
    for (std::size_t start = 0; start < count; ++start) {
        bool equal = true;
        for (std::size_t i = 0; i < count; ++i) {
            if (a.vertices[fa.vertices[i]] != b.vertices[fb.vertices[(start + i) % count]]) {
                equal = false;
                break;
            }
        }
        if (equal) return true;
    }
    return false;
}
vmf::Vec3 vector(DVec3 value) { return {value.x, value.y, value.z}; }
template <typename T> std::string geometryText(T value) {
    auto node = kv1::Node::value("", "");
    vmf::Value(&node).setGeometry(value).value();
    return node.decodedValue().value();
}
bool identity(std::string_view key, const kv1::Node& block) {
    return ciEquals(key, "id") ||
           (ciEquals(block.key(), "visgroup") && ciEquals(key, "visgroupid"));
}
// Managed descendants have their own arena values. Keep clean, empty placeholders in
// their codec slots so a parent snapshot never copies a whole subtree. Export always fills
// these slots before invoking the writer; the parent's own spans and trivia remain intact.
const kv1::Node& placeholder() {
    static const auto empty = kv1::parse("").value();
    return empty.root();
}
std::size_t nodeMemory(const kv1::Node& block) {
    std::size_t bytes = sizeof(block) + block.key().size() + block.condition().size();
    if (const auto raw = block.rawValue()) bytes += raw->size();
    for (const auto& child : block.children()) bytes += nodeMemory(child);
    return bytes;
}
std::vector<std::pair<std::size_t, NodeId>> rootSlots(const kv1::Document& source,
                                                     const std::vector<NodeId>& roots) {
    std::vector<std::pair<std::size_t, NodeId>> result;
    std::size_t n = 0;
    for (std::size_t i = 0; i < source.root().children().size(); ++i) {
        if (source.root().children()[i].kind() == kv1::NodeKind::Block)
            result.emplace_back(i, roots[n++]);
    }
    return result;
}
} // namespace

Document::Document(const vmf::VmfFile& file, std::size_t undoCap)
    : source_(file.kvDocument()), cap_(undoCap) {
    nextObject_ = std::max<std::int64_t>(1, std::int64_t(file.objectIds.maximum().value_or(0)) + 1);
    nextSide_ = std::max<std::int64_t>(1, std::int64_t(file.sideIds.maximum().value_or(0)) + 1);
    for (const auto& block : source_.root().children()) {
        if (block.kind() == kv1::NodeKind::Block) roots_.push_back(import(block, 0));
    }
    originalRoots_ = roots_;
    indexSides();
}

NodeId Document::import(const kv1::Node& block, NodeId parent) {
    auto value = typed(block.key());
    auto& n = base(value);
    const auto id = nextNode_++;
    n.id = id;
    n.parent = parent;
    auto shell = block;
    const auto* idValue = block.find(ciEquals(block.key(), "visgroup") ? "visgroupid" : "id");
    if (idValue) {
        const auto number = idValue->asInt();
        if (number) {
            auto* next = std::holds_alternative<Group>(value) ? &nextGroup_ :
                         std::holds_alternative<Visgroup>(value) ? &nextVisgroup_ :
                         (std::holds_alternative<Entity>(value) &&
                          !ciEquals(block.key(), "world")) || std::holds_alternative<Solid>(value)
                             ? &nextObject_ : nullptr;
            if (next) *next = std::max(*next, std::int64_t(*number) + 1);
        }
    }
    if (!std::holds_alternative<Solid>(value)) {
        for (std::size_t i = 0; i < block.children().size(); ++i) {
            const auto& child = block.children()[i];
            if (child.kind() == kv1::NodeKind::Block) {
                const auto childId = import(child, id);
                n.children.push_back(childId);
                n.sourceChildren.emplace_back(i, childId);
                shell.children()[i] = placeholder();
            }
        }
    }
    n.block = std::make_shared<const kv1::Node>(std::move(shell));
    put(id, value);
    return id;
}

const Node* Document::get(NodeHandle h) const {
    return std::visit([&](auto handle) -> const Node* { return get(handle); }, h);
}
std::optional<NodeHandle> Document::handle(NodeId id) const {
    const auto found = handles_.find(id);
    return found == handles_.end() ? std::nullopt : std::optional(found->second);
}
Node* Document::node(NodeId id) {
    return const_cast<Node*>(get(handles_.at(id)));
}
NodeValue Document::value(NodeId id) const {
    return std::visit([&](auto h) -> NodeValue { return *get(h); }, handles_.at(id));
}
void Document::put(NodeId id, const std::optional<NodeValue>& value) {
    const auto found = handles_.find(id);
    if (!value) {
        if (found != handles_.end()) {
            std::visit([&](auto h) {
                using T = std::remove_cvref_t<decltype(*get(h))>;
                std::get<Pool<T>>(pools_).destroy(h);
            }, found->second);
            handles_.erase(found);
        }
        return;
    }
    std::visit([&](const auto& n) {
        using T = std::remove_cvref_t<decltype(n)>;
        auto& arena = std::get<Pool<T>>(pools_);
        if (found == handles_.end()) handles_.emplace(id, arena.create(n));
        else *arena.get(std::get<Handle<T>>(found->second)) = n;
    }, *value);
}
int Document::allocate(std::int64_t& next) {
    if (next > std::numeric_limits<int>::max()) throw std::overflow_error("VMF ID space exhausted");
    return static_cast<int>(next++);
}

void Document::exportChildren(kv1::Node& block, const std::vector<NodeId>& children,
                             const std::vector<std::pair<std::size_t, NodeId>>& slots) const {
    if (children.size() == slots.size() &&
        std::equal(children.begin(), children.end(), slots.begin(),
                   [](NodeId id, const auto& slot) { return id == slot.second; })) {
        for (const auto& [index, id] : slots) block.children()[index] = exportNode(id);
        return;
    }
    block.setKey(std::string(block.key()));
    // Replace existing block slots in order; retain scalar keys and their trivia in place.
    std::size_t offset = 0, child = 0;
    for (const auto& [index, unused] : slots) {
        (void)unused;
        if (child < children.size()) {
            block.children()[index - offset] = exportNode(children[child++]);
        } else {
            block.remove(index - offset).value();
            ++offset;
        }
    }
    while (child < children.size()) block.append(exportNode(children[child++])).value();
}
kv1::Node Document::exportNode(NodeId id) const {
    const auto* n = get(handles_.at(id));
    auto block = *n->block;
    exportChildren(block, n->children, n->sourceChildren);
    return block;
}
Result<std::string> Document::write() const {
    auto output = source_;
    exportChildren(output.root(), roots_, rootSlots(source_, originalRoots_));
    return kv1::write(output);
}
Result<vmf::VmfFile> Document::toVmf() const {
    auto bytes = write();
    if (!bytes) return std::unexpected(bytes.error());
    return vmf::parse(*bytes, {source_.escapeMode()});
}
std::vector<NodeHandle> Document::dirtyNodes() const {
    std::vector<NodeHandle> result;
    for (const auto& [id, h] : handles_) {
        (void)id;
        if (get(h)->dirty != Dirty::None) result.push_back(h);
    }
    return result;
}
void Document::indexSides() {
    sides_.clear();
    sideCandidates_.clear();
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto h = *handle(id);
        if (const auto* solid = std::get_if<Handle<Solid>>(&h)) {
            std::size_t face = 0;
            for (const auto& child : get(h)->block->children()) {
                if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
                if (const auto* key = child.find("id")) {
                    if (const auto number = key->asInt()) {
                        const SideLocation location{*solid, face};
                        sides_.try_emplace(*number, location);
                        sideCandidates_[*number].push_back(location);
                    }
                }
                ++face;
            }
        }
        for (const auto child : get(h)->children) self(self, child);
    };
    for (const auto id : roots_) walk(walk, id);
}
void Document::refreshSide(SideId id) {
    const auto found = sideCandidates_.find(id);
    if (found == sideCandidates_.end()) {
        sides_.erase(id);
        return;
    }
    // Only malformed, duplicate IDs need hierarchy comparisons. Unique IDs take the fast path.
    const auto path = [&](Handle<Solid> solid) {
        std::vector<std::size_t> result;
        for (auto nodeId = get(solid)->id; nodeId; nodeId = node(nodeId)->parent) {
            const auto parent = node(nodeId)->parent;
            const auto& siblings = parent ? node(parent)->children : roots_;
            result.push_back(std::find(siblings.begin(), siblings.end(), nodeId) - siblings.begin());
        }
        std::reverse(result.begin(), result.end());
        return result;
    };
    const auto& candidates = found->second;
    auto first = candidates.front();
    for (std::size_t i = 1; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (candidate.solid == first.solid ? candidate.faceIndex < first.faceIndex :
            path(candidate.solid) < path(first.solid)) first = candidate;
    }
    sides_.insert_or_assign(id, first);
}
void Document::updateSolidSides(NodeId id, bool add) {
    const auto h = *handle(id);
    const auto* solid = std::get_if<Handle<Solid>>(&h);
    if (!solid) return;
    std::size_t face = 0;
    for (const auto& child : get(h)->block->children()) {
        if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
        if (const auto* key = child.find("id")) {
            if (const auto number = key->asInt()) {
                const SideLocation location{*solid, face};
                if (add) sideCandidates_[*number].push_back(location);
                else {
                    auto found = sideCandidates_.find(*number);
                    if (found != sideCandidates_.end()) {
                        std::erase(found->second, location);
                        if (found->second.empty()) sideCandidates_.erase(found);
                    }
                }
                refreshSide(*number);
            }
        }
        ++face;
    }
}
void Document::checkSideIndex() {
#ifndef NDEBUG
    std::map<SideId, SideLocation> rebuilt;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto h = *handle(id);
        if (const auto* solid = std::get_if<Handle<Solid>>(&h)) {
            std::size_t face = 0;
            for (const auto& child : get(h)->block->children()) {
                if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
                if (const auto* key = child.find("id")) {
                    if (const auto number = key->asInt())
                        rebuilt.try_emplace(*number, SideLocation{*solid, face});
                }
                ++face;
            }
        }
        for (const auto child : get(h)->children) self(self, child);
    };
    for (const auto id : roots_) walk(walk, id);
    assert(sides_ == rebuilt);
#endif
}
std::optional<SideLocation> Document::side(SideId id) const {
    const auto found = sides_.find(id);
    return found == sides_.end() ? std::nullopt : std::optional(found->second);
}

Transaction Document::begin(std::string label) { return Transaction(*this, std::move(label)); }
std::size_t Document::observe(Observer observer) {
    const auto token = nextObserver_++;
    observers_.emplace(token, std::move(observer));
    return token;
}
void Document::unobserve(std::size_t token) { observers_.erase(token); }
void Document::notify(const ChangeSet& changes) {
    const auto observers = observers_;
    notifying_ = true;
    try {
        for (const auto& [token, observer] : observers) {
            if (observers_.contains(token)) observer(changes);
        }
    } catch (...) { notifying_ = false; throw; }
    notifying_ = false;
}
ChangeSet Document::restore(const Entry& entry, bool after) {
    ChangeSet changes;
    const auto oldSides = sides_;
    for (const auto& delta : entry.deltas) {
        const auto old = handle(delta.id);
        const auto& target = after ? delta.after : delta.before;
        put(delta.id, target);
        const auto current = handle(delta.id);
        if (!old && current) changes.created.push_back(*current);
        else if (old && !current) changes.destroyed.push_back(*old);
        else if (old && current && delta.flags != Dirty::None)
            changes.modified.push_back({*current, delta.flags});
    }
    roots_ = after ? entry.rootsAfter : entry.rootsBefore;
    indexSides();
    checkSideIndex();
    for (const auto& [id, location] : oldSides) {
        (void)location;
        if (!sides_.contains(id)) changes.vanishedSides.push_back(id);
    }
    return changes;
}
std::size_t Document::memory(const Entry& entry) {
    std::size_t bytes = sizeof(Entry) + entry.label.size() +
                        (entry.rootsBefore.size() + entry.rootsAfter.size()) * sizeof(NodeId);
    if (entry.coalesce) bytes += entry.coalesce->second.size();
    for (const auto& delta : entry.deltas) {
        bytes += sizeof(Delta);
        for (const auto* v : {&delta.before, &delta.after}) {
            if (*v) {
                bytes += base(**v).children.size() * sizeof(NodeId) +
                         base(**v).sourceChildren.size() *
                             sizeof(std::pair<std::size_t, NodeId>) +
                         nodeMemory(*base(**v).block);
            }
        }
    }
    return bytes;
}
void Document::evict() {
    while (historyBytes_ > cap_ && (!undo_.empty() || !redo_.empty())) {
        // The front of undo is the oldest reachable entry. With no undo entries, evict
        // the farthest redo first, retaining a contiguous redo chain from the current state.
        auto& stack = undo_.empty() ? redo_ : undo_;
        historyBytes_ -= stack.front().bytes;
        stack.pop_front();
    }
}
void Document::setUndoCap(std::size_t bytes) { cap_ = bytes; evict(); }
bool Document::undo() {
    if (active_ || notifying_) throw std::logic_error("undo during a transaction/notification");
    if (undo_.empty()) return false;
    coalesceAllowed_ = false;
    auto entry = std::move(undo_.back()); undo_.pop_back();
    auto changes = restore(entry, false);
    redo_.push_back(std::move(entry));
    notify(changes);
    return true;
}
bool Document::redo() {
    if (active_ || notifying_) throw std::logic_error("redo during a transaction/notification");
    if (redo_.empty()) return false;
    coalesceAllowed_ = false;
    auto entry = std::move(redo_.back()); redo_.pop_back();
    auto changes = restore(entry, true);
    undo_.push_back(std::move(entry));
    notify(changes);
    return true;
}

Transaction::Transaction(Document& doc, std::string label) : doc_(&doc) {
    if (doc.active_ || doc.notifying_) throw std::logic_error("nested transaction/observer write");
    entry_.label = std::move(label);
    entry_.rootsBefore = doc.roots_;
    doc.active_ = true;
}
Transaction::Transaction(Transaction&& other) noexcept
    : doc_(std::exchange(other.doc_, nullptr)), entry_(std::move(other.entry_)),
      deltaIndex_(std::move(other.deltaIndex_)),
      key_(std::move(other.key_)), operations_(other.operations_) {}
Transaction::~Transaction() {
    if (doc_) {
        doc_->restore(entry_, false);
        doc_->active_ = false;
    }
}
NodeId Transaction::checked(NodeHandle h) const {
    if (!doc_) throw std::logic_error("transaction already finished");
    const auto* n = doc_->get(h);
    if (!n) throw std::invalid_argument("stale document handle");
    return n->id;
}
void Transaction::touch(NodeId id, Dirty flags) {
    const auto [found, inserted] = deltaIndex_.try_emplace(id, entry_.deltas.size());
    if (inserted) {
        entry_.deltas.push_back({id, doc_->value(id), {}, flags, doc_->handle(id)});
    } else {
        auto& delta = entry_.deltas[found->second];
        delta.flags = delta.flags | flags;
    }
    doc_->node(id)->dirty = doc_->node(id)->dirty | flags;
}
void Transaction::detach(NodeId id) {
    const auto parent = doc_->node(id)->parent;
    if (parent) touch(parent, Dirty::Hierarchy);
    auto& children = parent ? doc_->node(parent)->children : doc_->roots_;
    std::erase(children, id);
}
NodeHandle Transaction::create(kv1::Node block, std::optional<NodeHandle> parent) {
    if (!doc_) throw std::logic_error("transaction already finished");
    if (block.kind() != kv1::NodeKind::Block) throw std::invalid_argument("create needs a block");
    const auto parentId = parent ? checked(*parent) : 0;
    if (parent && std::holds_alternative<Handle<Solid>>(*parent))
        throw std::invalid_argument("sides belong inside a solid");
    // Allocate recursively before import, so imported IDs cannot advance a second time.
    const auto assign = [&](const auto& self, kv1::Node& b) -> void {
        const auto name = folded(std::string(b.key()));
        auto* next = name == "entity" || name == "solid" ? &doc_->nextObject_ :
                     name == "side" ? &doc_->nextSide_ : name == "group" ? &doc_->nextGroup_ :
                     name == "visgroup" ? &doc_->nextVisgroup_ : nullptr;
        if (next) set(b, name == "visgroup" ? "visgroupid" : "id",
                      std::to_string(Document::allocate(*next)));
        for (auto& child : b.children()) {
            if (child.kind() == kv1::NodeKind::Block) {
                self(self, child);
            }
        }
    };
    assign(assign, block);
    if (parentId) touch(parentId, Dirty::Hierarchy);
    const auto first = doc_->nextNode_;
    const auto id = doc_->import(block, parentId);
    for (auto created = first; created < doc_->nextNode_; ++created) {
        doc_->node(created)->dirty = Dirty::Hierarchy;
        deltaIndex_.emplace(created, entry_.deltas.size());
        entry_.deltas.push_back({created, {}, {}, Dirty::Hierarchy, {}});
    }
    auto& children = parentId ? doc_->node(parentId)->children : doc_->roots_;
    children.push_back(id);
    ++operations_;
    for (auto created = first; created < doc_->nextNode_; ++created)
        doc_->updateSolidSides(created, true);
    return *doc_->handle(id);
}
void Transaction::destroyId(NodeId id) {
    const auto children = doc_->node(id)->children;
    for (const auto child : children) destroyId(child);
    touch(id, Dirty::Hierarchy);
    doc_->updateSolidSides(id, false);
    doc_->put(id, {});
}
void Transaction::destroy(NodeHandle h) {
    const auto id = checked(h);
    detach(id);
    destroyId(id);
    ++operations_;
}
void Transaction::reparent(NodeHandle h, std::optional<NodeHandle> parent, std::size_t position) {
    const auto id = checked(h);
    const auto parentId = parent ? checked(*parent) : 0;
    if (parent && std::holds_alternative<Handle<Solid>>(*parent))
        throw std::invalid_argument("sides belong inside a solid");
    for (auto ancestor = parentId; ancestor; ancestor = doc_->node(ancestor)->parent)
        if (ancestor == id) throw std::invalid_argument("hierarchy cycle");
    touch(id, Dirty::Hierarchy);
    detach(id);
    if (parentId) touch(parentId, Dirty::Hierarchy);
    auto& children = parentId ? doc_->node(parentId)->children : doc_->roots_;
    position = std::min(position, children.size());
    children.insert(children.begin() + static_cast<std::ptrdiff_t>(position), id);
    doc_->node(id)->parent = parentId;
    ++operations_;
    const auto refresh = [&](const auto& self, NodeId moved) -> void {
        doc_->updateSolidSides(moved, false);
        doc_->updateSolidSides(moved, true);
        for (const auto child : doc_->node(moved)->children) self(self, child);
    };
    refresh(refresh, id);
}
void Transaction::setKeyValue(NodeHandle h, std::string key, std::optional<std::string> text) {
    const auto id = checked(h);
    const auto& original = *doc_->node(id)->block;
    if (identity(key, original)) {
        throw std::invalid_argument("identity keys are allocated by Document");
    }
    if (original.findAll(key).size() > 1)
        throw std::invalid_argument("setKeyValue requires a unique key");
    std::optional<std::size_t> removed;
    bool found = false;
    for (std::size_t i = 0; i < original.children().size(); ++i) {
        const auto& child = original.children()[i];
        if (child.kind() != kv1::NodeKind::Value || !ciEquals(child.key(), key)) continue;
        if (text && child.decodedValue() == *text) return;
        if (!text) removed = i;
        found = true;
        break;
    }
    if (!found && !text) return;
    auto block = original;
    set(block, key, text);
    const auto flags = ciEquals(key, "visgroupid") || ciEquals(key, "visgroupshown") ||
                       ciEquals(key, "visgroupautoshown") ? Dirty::Visibility : Dirty::KeyValues;
    touch(id, flags);
    if (removed) {
        for (auto& [index, child] : doc_->node(id)->sourceChildren) {
            (void)child;
            if (index > *removed) --index;
        }
    }
    doc_->node(id)->block = std::make_shared<const kv1::Node>(std::move(block));
    key_ = {id, folded(std::move(key))};
    ++operations_;
}
void Transaction::setEditor(NodeHandle h, EditorEdit edit) {
    const auto id = checked(h);
    const bool solid = std::holds_alternative<Handle<Solid>>(h);
    if (!solid && !std::holds_alternative<Handle<Entity>>(h) &&
        !std::holds_alternative<Handle<Group>>(h))
        throw std::invalid_argument("setEditor needs a solid, entity or group");
    NodeId editorId = 0;
    std::optional<std::size_t> inlineIndex;
    auto editor = kv1::Node::block("editor");
    if (solid) {
        const auto& children = doc_->node(id)->block->children();
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (children[i].kind() == kv1::NodeKind::Block && ciEquals(children[i].key(), "editor")) {
                inlineIndex = i;
                editor = children[i];
                break;
            }
        }
    } else {
        for (const auto child : doc_->node(id)->children) {
            if (ciEquals(doc_->node(child)->block->key(), "editor")) {
                editorId = child;
                editor = *doc_->node(child)->block;
                break;
            }
        }
    }
    std::optional<std::size_t> removedIndex;
    std::string key;
    Dirty flags = Dirty::KeyValues;
    switch (edit.kind) {
    case EditorEdit::Kind::Group: key = "groupid"; flags = Dirty::Hierarchy; break;
    case EditorEdit::Kind::AddVisgroup:
    case EditorEdit::Kind::RemoveVisgroup: key = "visgroupid"; flags = Dirty::Visibility; break;
    case EditorEdit::Kind::Color: key = "color"; break;
    case EditorEdit::Kind::VisgroupShown: key = "visgroupshown"; flags = Dirty::Visibility; break;
    case EditorEdit::Kind::VisgroupAutoShown:
        key = "visgroupautoshown"; flags = Dirty::Visibility; break;
    case EditorEdit::Kind::Comments: key = "comments"; break;
    }
    if (edit.kind == EditorEdit::Kind::AddVisgroup) {
        editor.append(kv1::Node::value(key, *edit.value)).value();
    } else if (edit.kind == EditorEdit::Kind::RemoveVisgroup) {
        bool removed = false;
        for (std::size_t i = 0; i < editor.children().size(); ++i) {
            const auto& child = editor.children()[i];
            if (child.kind() == kv1::NodeKind::Value && ciEquals(child.key(), key) &&
                child.asInt() == std::stoi(*edit.value)) {
                removedIndex = i;
                editor.remove(i).value();
                removed = true;
                break;
            }
        }
        if (!removed) return;
    } else {
        const auto* previous = editor.find(key);
        if ((!previous && !edit.value) ||
            (previous && edit.value && previous->decodedValue() == *edit.value)) return;
        if (!edit.value) {
            for (std::size_t i = 0; i < editor.children().size(); ++i) {
                if (&editor.children()[i] == previous) removedIndex = i;
            }
        }
        set(editor, key, edit.value);
    }
    touch(id, flags);
    if (solid) {
        auto block = *doc_->node(id)->block;
        if (inlineIndex) block.children()[*inlineIndex] = std::move(editor);
        else block.append(std::move(editor)).value();
        doc_->node(id)->block = std::make_shared<const kv1::Node>(std::move(block));
    } else if (editorId) {
        // The child snapshot is needed for exact undo; observers consume the object's flags.
        touch(editorId, Dirty::None);
        if (removedIndex) {
            for (auto& [index, child] : doc_->node(editorId)->sourceChildren) {
                (void)child;
                if (index > *removedIndex) --index;
            }
        }
        doc_->node(editorId)->block = std::make_shared<const kv1::Node>(std::move(editor));
    } else {
        editorId = doc_->import(editor, id);
        deltaIndex_.emplace(editorId, entry_.deltas.size());
        entry_.deltas.push_back({editorId, {}, {}, Dirty::None, {}});
        doc_->node(id)->children.push_back(editorId);
    }
    ++operations_;
}
void Transaction::setSideKeyValue(SideId sideId, std::string key, std::string text) {
    if (!doc_) throw std::logic_error("transaction already finished");
    const auto location = doc_->side(sideId);
    if (!location) throw std::invalid_argument("unknown side ID");
    const auto id = checked(location->solid);
    const auto faceIndex = location->faceIndex;
    if (identity(key, *doc_->node(id)->block))
        throw std::invalid_argument("side IDs are allocated by Document");
    auto block = *doc_->node(id)->block;
    std::size_t face = 0;
    for (auto& child : block.children()) {
        if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
        if (face++ != faceIndex) continue;
        set(child, key, text);
        const auto flags = ciEquals(key, "material") ? Dirty::Material :
                           ciEquals(key, "plane") ? Dirty::Geometry : Dirty::KeyValues;
        touch(id, flags);
        doc_->node(id)->block = std::make_shared<const kv1::Node>(std::move(block));
        key_ = {id, "side:" + std::to_string(faceIndex) + ":" + folded(std::move(key))};
        ++operations_;
        return;
    }
    throw std::invalid_argument("side index out of range");
}
void Transaction::setSolidGeometry(Handle<Solid> h, const geometry::SolidGeometry& geometry) {
    const auto id = checked(h);
    const auto& original = *doc_->node(id)->block;
    auto before = geometry::solidGeometry(original);
    if (!before) throw std::invalid_argument(before.error().what());
    const auto sideIds = [](const auto& sides) {
        std::set<std::uint32_t> ids;
        for (const auto& side : sides) {
            if (!ids.insert(side.sideId).second)
                throw std::invalid_argument("geometry requires unique side IDs");
        }
        return ids;
    };
    if (sideIds(before->sides) != sideIds(geometry.sides) ||
        sideIds(before->poly.faces) != sideIds(geometry.poly.faces))
        throw std::invalid_argument("geometry side IDs do not match solid");
    if (!geometry.poly.isValid()) throw std::invalid_argument("invalid solid geometry");

    std::map<std::uint32_t, const geometry::Face*> oldFaces, newFaces;
    for (const auto& face : before->poly.faces) oldFaces.emplace(face.sideId, &face);
    for (const auto& face : geometry.poly.faces) newFaces.emplace(face.sideId, &face);
    auto block = original;
    bool changed = false;
    for (auto* side : block.findAll("side")) {
        const auto sideId = static_cast<std::uint32_t>(side->find("id")->asInt().value());
        const auto found = newFaces.find(sideId);
        // Redundant input planes have no boundary face; leave those source sides intact.
        if (found == newFaces.end()) continue;
        const auto& face = *found->second;
        const auto& old = *oldFaces.at(sideId);
        const bool polygonChanged = !samePolygon(before->poly, old, geometry.poly, face);
        const bool uvChanged = !sameUv(old.uv, face.uv);
        if (!polygonChanged && !uvChanged) continue;
        if (polygonChanged) {
            // A redundant source side has no vertices to transform. Identity is safe,
            // but retaining its old plane during a move could clip the edited brush.
            if (newFaces.size() != before->sides.size())
                throw std::invalid_argument("geometry side IDs have no matching boundary faces");
            const auto points = geometry.poly.sourcePlanePoints(face);
            if (!points) throw std::invalid_argument("face has no Source plane points");
            vmf::PlanePoints plane;
            for (std::size_t i = 0; i < plane.points.size(); ++i) plane.points[i] = vector((*points)[i]);
            vmf::Value(side->find("plane")).set(plane).value();
            auto data = kv1::Node::block("point_data");
            data.append(kv1::Node::value("numpts", std::to_string(face.vertices.size()))).value();
            // The kernel loop is counter-clockwise; Source point_data is clockwise.
            std::size_t index = 0;
            for (auto vertex = face.vertices.rbegin(); vertex != face.vertices.rend(); ++vertex) {
                data.append(kv1::Node::value("point", std::to_string(index++) + " " +
                    geometryText(vector(geometry.poly.vertices[*vertex])))).value();
            }
            if (auto* existing = side->find("point_data")) *existing = std::move(data);
            else side->append(std::move(data)).value();
        }
        if (old.uv.uaxis != face.uv.uaxis || old.uv.shiftU != face.uv.shiftU ||
            old.uv.scaleU != face.uv.scaleU) {
            set(*side, "uaxis", geometryText(vmf::UvAxis{vector(face.uv.uaxis), face.uv.shiftU,
                                                        face.uv.scaleU}));
        }
        if (old.uv.vaxis != face.uv.vaxis || old.uv.shiftV != face.uv.shiftV ||
            old.uv.scaleV != face.uv.scaleV) {
            set(*side, "vaxis", geometryText(vmf::UvAxis{vector(face.uv.vaxis), face.uv.shiftV,
                                                        face.uv.scaleV}));
        }
        changed = true;
    }
    if (!changed) return;
    touch(id, Dirty::Geometry);
    doc_->node(id)->block = std::make_shared<const kv1::Node>(std::move(block));
    ++operations_;
}
ChangeSet Transaction::commit() {
    if (!doc_) throw std::logic_error("transaction already finished");
    for (auto& delta : entry_.deltas) {
        if (doc_->handle(delta.id)) delta.after = doc_->value(delta.id);
    }
    std::erase_if(entry_.deltas, [](const auto& d) { return !d.before && !d.after; });
    entry_.rootsAfter = doc_->roots_;
    if (operations_ == 1) entry_.coalesce = key_;
    // Build the event from the before snapshots without disturbing live handles.
    ChangeSet changes;
    std::map<SideId, bool> previousSides;
    for (const auto& delta : entry_.deltas) {
        const auto h = doc_->handle(delta.id);
        if (!delta.before && h) changes.created.push_back(*h);
        else if (delta.before && h && delta.flags != Dirty::None)
            changes.modified.push_back({*h, delta.flags});
        else if (delta.before && !h) {
            changes.destroyed.push_back(*delta.beforeHandle);
        }
        if (delta.before && std::holds_alternative<Solid>(*delta.before)) {
            for (const auto& side : base(*delta.before).block->children()) {
                if (side.kind() != kv1::NodeKind::Block || !ciEquals(side.key(), "side")) continue;
                if (const auto* key = side.find("id")) {
                    if (const auto number = key->asInt()) previousSides.emplace(*number, true);
                }
            }
        }
    }
    for (const auto& [id, unused] : previousSides) {
        (void)unused;
        if (!doc_->side(id)) changes.vanishedSides.push_back(id);
    }
    if (!entry_.deltas.empty()) {
        for (const auto& redo : doc_->redo_) doc_->historyBytes_ -= redo.bytes;
        doc_->redo_.clear();
        if (doc_->coalesceAllowed_ && entry_.coalesce && !doc_->undo_.empty() &&
            doc_->undo_.back().coalesce == entry_.coalesce) {
            auto& previous = doc_->undo_.back();
            doc_->historyBytes_ -= previous.bytes;
            previous.deltas.front().after = entry_.deltas.front().after;
            previous.rootsAfter = entry_.rootsAfter;
            previous.bytes = Document::memory(previous);
            doc_->historyBytes_ += previous.bytes;
        } else {
            entry_.bytes = Document::memory(entry_);
            doc_->historyBytes_ += entry_.bytes;
            doc_->undo_.push_back(std::move(entry_));
        }
        doc_->coalesceAllowed_ = true;
        doc_->evict();
    }
    doc_->checkSideIndex();
    auto* doc = std::exchange(doc_, nullptr);
    doc->active_ = false;
    if (!changes.empty()) doc->notify(changes);
    return changes;
}

} // namespace apex::document
