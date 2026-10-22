#pragma once

#include "editor/group_ops.hpp"

namespace apex::editor {

// Selected objects with flat group membership expanded. Faces use their owning solid.
std::vector<NodeId> visibilityTargets(const document::Document& doc, const GroupIndex& groups,
                                const Selection& selection);
Result<EditedObjects> stageHide(const document::Document& doc, const GroupIndex& groups,
                                const Selection& selection, document::Transaction& tx);
Result<EditedObjects> stageSetNodeHidden(const document::Document& doc, const GroupIndex& groups,
                                         const std::vector<NodeId>& nodes, bool hidden,
                                         document::Transaction& tx);
Result<EditedObjects> stageUnhideAll(const document::Document& doc, document::Transaction& tx);
Result<EditedObjects> stageHideUnselected(const document::Document& doc, const GroupIndex& groups,
                                          const Selection& selection, document::Transaction& tx);

} // namespace apex::editor
