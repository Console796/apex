#pragma once

#include "core/entity_class.hpp"
#include "editor/selection.hpp"

namespace apex::editor {

// A snapshot: rebuild after document or selection changes. Rows own metadata and values.
class InspectorModel {
public:
    enum class Section { Typed, Other, Raw };
    struct Row {
        std::string key, value;
        std::optional<assets::EntityProperty> property;
        Section section = Section::Other;
        bool mixed = false;
        bool readOnly = false;
        // Zero-based occurrence among scalar keys with this name, for lossless raw editing.
        std::size_t occurrence = 0;
    };
    InspectorModel(const document::Document& doc, const Selection& selection,
                   const assets::EntityClassTable& classes, bool raw = false);
    const std::vector<Row>& rows() const { return rows_; }
    const std::vector<NodeId>& entities() const { return entities_; }
    const std::vector<std::string>& targetCandidates() const { return targets_; }
    std::vector<std::string> autocomplete(std::size_t row) const;
    // Stage into the caller-owned transaction; drop it on failure to roll back.
    Result<void> setProperty(document::Transaction& tx, std::size_t row, std::string text) const;
    Result<void> setVectorComponent(document::Transaction& tx, std::size_t row, std::size_t component, std::string text) const;
    Result<void> toggleFlag(document::Transaction& tx, std::size_t row, std::uint64_t bit, bool enabled) const;
    Result<void> setChoice(document::Transaction& tx, std::size_t row, std::string value) const;
    Result<void> setClass(document::Transaction& tx, std::string classname) const;

private:
    const document::Document& doc_;
    std::vector<NodeId> entities_;
    std::vector<Row> rows_;
    std::vector<std::string> targets_;
    std::map<std::pair<NodeId, std::string>, std::string> defaults_;
    Result<void> writable(std::size_t row) const;
};

} // namespace apex::editor
