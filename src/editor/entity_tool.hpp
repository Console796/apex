#pragma once

#include "editor/tool.hpp"

namespace apex::editor {

class EntityTool final : public Tool {
public:
    explicit EntityTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.entity"; }
    Handled input(const InputEvent& event, const ToolView& view) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView&, InputSource source) const override;
    ToolReadout readout() const override;
    void cancel() override { selectionGesture_.clear(); preview_.clear(); }
    void forget(InputSource source) override { selectionGesture_.forget(source); }
    void pointerHover(Ray ray) override;
    void pointerClick(Ray ray, Modifiers modifiers, bool sameSpot) override;
private:
    void clearGhost();
    EditorSession& session_;
    SelectionGesture selectionGesture_;
    Preview preview_;
};

} // namespace apex::editor
