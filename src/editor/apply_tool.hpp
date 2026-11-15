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
    bool setParameter(std::string_view id, double value) override;
    // Plan 0.8 D13: in Hotspot mode an apply also fits the face to a hotspot rect of the applied
    // material (EditorSession::applyHotspotAt); Alt then picks the alternate group instead of wrap.
    bool hotspot() const { return hotspot_; }
    void setHotspot(bool hotspot) { hotspot_ = hotspot; }
private:
    EditorSession& session_;
    Preview preview_;
    std::string notice_;
    bool hotspot_ = false;
};
} // namespace apex::editor
