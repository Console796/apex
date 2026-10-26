#pragma once

#include "editor/snapper.hpp"
#include "editor/tool.hpp"

namespace apex::editor {

class ClipTool final : public Tool {
public:
    explicit ClipTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.clip"; }
    Handled input(const InputEvent& event, const ToolView& view) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView& view, InputSource source) const override;
    ToolReadout readout() const override;
    std::vector<ActionId> actions() const override { return {"clip.cycle_keep"}; }
    bool perform(const ActionId& action) override;
    void cancel() override;
    void forget(InputSource source) override;
    bool owns(InputSource source) const override;
private:
    enum class Keep { Front, Back, Both };
    struct Piece {
        Handle<document::Solid> solid;
        geometry::ConvexPolyhedron front, back;
    };
    struct Gesture {
        std::vector<DVec3> points;
        std::optional<std::size_t> dragging;
        std::optional<DVec3> viewDirection;
        std::optional<SnapResult> snapped;
        std::vector<Piece> pieces;
        std::string warning;
    };
    EditorSession& session_;
    std::map<InputSource, Gesture> gestures_;
    std::optional<InputSource> current_;
    Keep keep_ = Keep::Front;
    Preview preview_;
    SnapResult snap(const InputEvent& event, const ToolView& view) const;
    void rebuild(Gesture& gesture);
    void updatePreview();
    void publish();
    void stepBack(InputSource source);
    bool commit(InputSource source);
};

} // namespace apex::editor
