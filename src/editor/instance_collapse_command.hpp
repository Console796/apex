#pragma once

#include "editor/editor_session.hpp"

namespace apex::editor {

Result<InstanceCollapseReport> stageInstanceCollapse(EditorSession& session, document::Document& doc,
                                                      std::span<const NodeId> instances, bool all);

} // namespace apex::editor
