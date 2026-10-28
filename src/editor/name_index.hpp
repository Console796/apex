#pragma once

#include "document/document.hpp"

#include <map>
#include <set>
#include <string_view>

namespace apex::editor {

// Entity targetnames use exact ASCII case-insensitive lookup. The document must
// outlive this index, including its optional synchronous observer.
class NameIndex {
public:
    explicit NameIndex(const document::Document& document);
    ~NameIndex();
    NameIndex(const NameIndex&) = delete;
    NameIndex& operator=(const NameIndex&) = delete;
    NameIndex(NameIndex&&) = delete;
    NameIndex& operator=(NameIndex&&) = delete;

    void rebuild();
    void apply(const document::ChangeSet& changes);
    void attach(document::Document& document);
    void detach();
    const document::Document& document() const { return doc_; }
    std::vector<document::NodeId> find(std::string_view name) const;
    // Only a single leading and/or trailing '*' is supported; interior stars do not match.
    std::vector<document::NodeId> matching(std::string_view pattern) const;
    // Sorted by lower-case name, retaining each name's first-seen spelling.
    std::vector<std::string> names() const;
    std::size_t size() const { return names_.size(); }

private:
    struct Entry {
        std::string spelling;
        std::set<document::NodeId> nodes;
    };
    const document::Document& doc_;
    std::map<std::string, Entry> names_;
    std::map<document::NodeId, std::string> records_;
    std::map<document::NodeHandle, document::NodeId> handles_;
    document::Document* observed_ = nullptr;
    std::size_t observer_ = 0;

    void refresh(document::NodeId id);
};

} // namespace apex::editor
