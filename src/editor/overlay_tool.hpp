#pragma once

// Plan 0.8 D12: the Overlay tool. A click on a face places an info_overlay with the active material
// (or, in Decal mode, an infodecal with that texture) and selects it. The selected overlay shows
// handles that move, rotate and scale its quad or drag one of its four UV corners; each drag is one
// undo step. Shift-click on a face adds it to or removes it from the selected overlay's side list.
// The placement and drag maths are in overlay_placement.hpp.

#include "editor/overlay_placement.hpp"
#include "editor/tool.hpp"

namespace apex::editor {

class OverlayTool final : public Tool {
public:
    enum class Mode { Overlay, Decal };
    explicit OverlayTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.overlay"; }
    void activate() override;
    Handled input(const InputEvent& event, const ToolView& view) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView& view, InputSource source) const override;
    ToolReadout readout() const override;
    void cancel() override;
    bool setParameter(std::string_view id, double value) override;
    void documentChanged() override;

    Mode mode() const { return mode_; }
    void setMode(Mode mode);

private:
    struct Target {
        NodeId node = 0;
        geometry::OverlayQuad quad;
        std::vector<int> sides;
    };
    struct Drag {
        NodeId node = 0;
        OverlayHandle handle;
        geometry::OverlayQuad start;
        std::vector<int> startSides;
        DVec3 from{0.0};
        std::vector<document::OverlaySurface> surfaces;
        std::string coalesceKey;
    };
    std::optional<Target> target() const;
    std::optional<OverlayHandle> handleAt(const Target& target, const ToolView& view, DVec2 cursor) const;
    void place(const document::OverlaySurface& face, DVec3 point);
    void toggleFace(const Target& target, SideId side);
    void dragTo(DVec3 point, bool finish);
    void writeQuad(NodeId node, const geometry::OverlayQuad& quad, const std::vector<int>& sides,
                   const std::string& label, const std::string& coalesceKey);
    void setNotice(std::string notice);

    EditorSession& session_;
    Preview preview_;
    Mode mode_ = Mode::Overlay;
    std::optional<Drag> drag_;
    std::optional<OverlayHandle> hovered_;
    std::string notice_;
    std::uint64_t drags_ = 0;
};

} // namespace apex::editor
