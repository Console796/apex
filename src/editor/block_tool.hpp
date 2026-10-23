#pragma once

#include "editor/snapper.hpp"
#include "editor/tool.hpp"

namespace apex::editor {

class BlockTool final : public Tool {
public:
    explicit BlockTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.block"; }
    Handled input(const InputEvent& event, const ToolView& view) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView& view, InputSource source) const override;
    ToolReadout readout() const override;
    void cancel() override;
    void forget(InputSource source) override;
    bool owns(InputSource source) const override;
private:
    enum class Stage { Base, Height };
    struct Gesture {
        Stage stage = Stage::Base;
        SurfacePlane plane;
        DVec2 start{0.0}, end{0.0}, heightAnchor{0.0};
        bool dragging = false;
        double mouseHeight = 0.0, height = 0.0, initialHeight = 0.0;
        std::string typed;
        std::optional<std::pair<DVec3, SnapKind>> cornerSnap;
        std::optional<geometry::ConvexPolyhedron> poly;
    };
    EditorSession& session_;
    std::map<InputSource, Gesture> gestures_;
    std::optional<InputSource> current_;
    std::optional<double> lastHeight_;
    std::optional<SurfacePlane> lastPlane_;
    std::map<InputSource, bool> hoverSuspended_;
    bool hoveringLastPlane_ = false;
    Preview preview_;
    SurfacePlane basePlane(const ToolView& view, bool* remembered = nullptr) const;
    void rebuild(Gesture& gesture);
    void updateGhosts();
    void drop(InputSource source); // forget() plus a notify, for input paths.
    void publish();
    bool commit(InputSource source);
};

} // namespace apex::editor
