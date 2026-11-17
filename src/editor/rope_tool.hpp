#pragma once

#include "editor/tool.hpp"

namespace apex::editor {

// Collect snapped points without editing the map; Enter creates the entire chain in one edit.
class RopeTool final : public Tool {
public:
    explicit RopeTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.rope"; }
    void activate() override;
    Handled input(const InputEvent& event, const ToolView& view) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView&, InputSource) const override;
    ToolReadout readout() const override;
    void cancel() override;
    void documentChanged() override { cancel(); }
private:
    void commit();
    EditorSession& session_;
    Preview preview_;
    std::vector<DVec3> points_;
    std::string notice_;
};

} // namespace apex::editor
