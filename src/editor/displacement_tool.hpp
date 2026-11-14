#pragma once

// Plan 0.8 D6: the Displacement tool. Its readout picks a mode: Create (power and elevation),
// Subdivide or Sew. A click selects faces as in the face edit tool (Ctrl toggles, Shift adds);
// Enter, or the "displacement.apply" action, applies the mode to the selected faces as one undo
// step. Sculpt and paint modes come in 0.8.4. The edits are in displacement_edit.hpp.

#include "editor/tool.hpp"

namespace apex::editor {

class DisplacementTool final : public Tool {
public:
    enum class Mode { Create, Subdivide, Sew };
    explicit DisplacementTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.displacement"; }
    void activate() override;
    Handled input(const InputEvent& event, const ToolView& view) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView& view, InputSource source) const override;
    ToolReadout readout() const override;
    void cancel() override {}
    bool setParameter(std::string_view id, double value) override;
    bool perform(const ActionId& action) override;

    Mode mode() const { return mode_; }
    void setMode(Mode mode);
    int power() const { return power_; }
    double elevation() const { return elevation_; }
    // Applies the mode to the selected faces; false (with the reason in the readout) when it did nothing.
    bool apply();
    const std::string& notice() const { return notice_; }

private:
    void setNotice(std::string notice);
    EditorSession& session_;
    Preview preview_;
    Mode mode_ = Mode::Create;
    int power_ = 3;
    double elevation_ = 0.0;
    std::string notice_;
};

} // namespace apex::editor
