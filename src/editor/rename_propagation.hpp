#pragma once

#include "core/entity_class.hpp"
#include "document/document.hpp"

namespace apex::editor {

struct RenameChange {
    enum class Kind { Connection, Key };
    document::NodeId entity = 0;
    Kind kind = Kind::Connection;
    std::string where;
    std::size_t index = 0;
    std::string before, after;
};
struct RenamePlan {
    std::string oldName, newName;
    std::vector<RenameChange> changes;
    std::map<document::NodeId, std::string> entityLabels;
};

RenamePlan planRename(const document::Document& doc, const assets::EntityClassTable& classes,
                      document::NodeId entity, std::string_view newName);
Result<void> stageRename(const document::Document& doc, const RenamePlan& plan, document::NodeId entity,
                         document::Transaction& tx);
std::vector<std::string> describe(const RenamePlan& plan);

} // namespace apex::editor
