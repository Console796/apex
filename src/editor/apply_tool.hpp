#pragma once
#include "editor/tool.hpp"

namespace apex::editor {
// True when right click applies (Hammer); false when a click modifier does (Blender's apply.click,
// which leaves right click to the context menu).
bool rightClickApplies(const Preset& preset);
class ApplyTool final : public Tool {
public:
    explicit ApplyTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.apply"; }
    void activate() override;
    Handled input(const InputEvent&, const ToolView&) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView&, InputSource) const override { return {}; }
    ToolReadout readout() const override;
    void cancel() override {}
private:
    EditorSession& session_;
    Preview preview_;
    std::string notice_;
};
} // namespace apex::editor
