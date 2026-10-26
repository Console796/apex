#pragma once

#include "document/connection_codec.hpp"
#include "editor/selection.hpp"

namespace apex::editor {

// A snapshot: rebuild after document or selection changes. Entry indices are block-local.
class ConnectionModel {
public:
    using Field = document::ConnectionField;
    struct Row {
        std::string output;
        document::Connection connection;
        std::size_t index = 0;
        // Parallel to entities(), including each entity's original entry index.
        std::vector<std::size_t> indices;
    };
    ConnectionModel(const document::Document& doc, const Selection& selection);
    const std::vector<Row>& rows() const { return rows_; }
    const std::vector<NodeId>& entities() const { return entities_; }
    Result<void> setField(document::Transaction& tx, std::size_t row, Field field, std::string text) const;
    Result<void> setOutput(document::Transaction& tx, std::size_t row, std::string name) const;
    Result<void> add(document::Transaction& tx, std::string output, document::Connection connection,
                     std::optional<std::size_t> after = {}) const;
    Result<void> remove(document::Transaction& tx, std::size_t row) const;
    Result<void> duplicate(document::Transaction& tx, std::size_t row) const;

private:
    const document::Document& doc_;
    std::vector<NodeId> entities_;
    std::vector<Row> rows_;
    Result<void> edit(document::Transaction& tx, std::optional<std::size_t> row,
                      const std::function<bool(kv1::Node&, std::size_t)>& stage,
                      std::optional<std::size_t> after = {}) const;
};

} // namespace apex::editor
