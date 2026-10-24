#pragma once

#include "editor/draw_extrude_tool.hpp"

namespace apex::editor {
class BlockTool final : public DrawExtrudeTool {
public:
    using DrawExtrudeTool::DrawExtrudeTool;
    ToolId id() const override { return "tool.block"; }
};
} // namespace apex::editor
