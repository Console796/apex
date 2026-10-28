#include "editor/name_index.hpp"

#include "core/strings.hpp"

#include <stdexcept>

namespace apex::editor {

NameIndex::NameIndex(const document::Document& doc) : doc_(doc) { rebuild(); }
NameIndex::~NameIndex() { detach(); }

void NameIndex::attach(document::Document& doc) {
    if (&doc != &doc_) throw std::invalid_argument("NameIndex observer document differs");
    if (observed_) return;
    rebuild();
    observer_ = doc.observe([this](const document::ChangeSet& changes) { apply(changes); });
    observed_ = &doc;
}

void NameIndex::detach() {
    if (observed_) observed_->unobserve(observer_);
    observed_ = nullptr;
}

void NameIndex::refresh(document::NodeId id) {
    const auto h = doc_.handle(id);
    const auto* node = h ? doc_.get(*h) : nullptr;
    std::string spelling;
    if (node && std::holds_alternative<Handle<document::Entity>>(*h) && node->block) {
        handles_[*h] = id;
        if (const auto* value = node->block->find("targetname"))
            spelling = value->decodedValue().value_or("");
    }
    const auto key = toLowerAscii(spelling);
    if (const auto old = records_.find(id); old != records_.end()) {
        if (old->second == key) return;
        auto& entry = names_.at(old->second);
        entry.nodes.erase(id);
        if (entry.nodes.empty()) names_.erase(old->second);
        records_.erase(old);
    }
    if (key.empty()) return;
    auto [entry, inserted] = names_.try_emplace(key, Entry{spelling, {}});
    (void)inserted;
    entry->second.nodes.insert(id);
    records_.emplace(id, key);
}

void NameIndex::rebuild() {
    names_.clear();
    records_.clear();
    handles_.clear();
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        refresh(id);
        const auto h = doc_.handle(id);
        if (const auto* node = h ? doc_.get(*h) : nullptr) {
            for (const auto child : node->children) self(self, child);
        }
    };
    for (const auto root : doc_.roots()) walk(walk, root);
}

void NameIndex::apply(const document::ChangeSet& changes) {
    std::set<document::NodeId> affected;
    const auto touch = [&](document::NodeHandle h) {
        if (const auto old = handles_.find(h); old != handles_.end()) affected.insert(old->second);
        if (const auto* node = doc_.get(h)) affected.insert(node->id);
    };
    for (const auto h : changes.created) touch(h);
    for (const auto& m : changes.modified) {
        if ((static_cast<unsigned>(m.flags) & static_cast<unsigned>(document::Dirty::KeyValues)) != 0)
            touch(m.handle);
    }
    for (const auto h : changes.destroyed) touch(h);
    for (const auto h : changes.destroyed) handles_.erase(h);
    for (const auto id : affected) refresh(id);
}

std::vector<document::NodeId> NameIndex::find(std::string_view name) const {
    const auto it = names_.find(toLowerAscii(name));
    if (it == names_.end()) return {};
    return {it->second.nodes.begin(), it->second.nodes.end()};
}

std::vector<document::NodeId> NameIndex::matching(std::string_view pattern) const {
    if (pattern.find('*') == std::string_view::npos) return find(pattern);
    const bool leading = pattern.starts_with('*'), trailing = pattern.ends_with('*');
    if (leading) pattern.remove_prefix(1);
    if (trailing && !pattern.empty()) pattern.remove_suffix(1);
    if (pattern.find('*') != std::string_view::npos) return {};
    const auto key = toLowerAscii(pattern);
    std::set<document::NodeId> result;
    for (const auto& [name, entry] : names_) {
        const bool match = leading && trailing ? name.find(key) != std::string::npos :
                           leading ? name.ends_with(key) : name.starts_with(key);
        if (match) result.insert(entry.nodes.begin(), entry.nodes.end());
    }
    return {result.begin(), result.end()};
}

std::vector<std::string> NameIndex::names() const {
    std::vector<std::string> result;
    for (const auto& [key, entry] : names_) {
        (void)key;
        result.push_back(entry.spelling);
    }
    return result;
}

} // namespace apex::editor
