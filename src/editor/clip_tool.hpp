#pragma once

#include "editor/snapper.hpp"
#include "editor/tool.hpp"

#include <set>
#include <span>

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
    struct Source {
        Handle<document::Solid> solid;
        geometry::ConvexPolyhedron poly;
    };
    // Points 1 and 2 are dragged out. Point 3 is inferred from the face the line lies on (or the
    // view direction in orthographic views) until the user places or drags it.
    struct Gesture {
        std::vector<DVec3> points;
        std::optional<std::size_t> dragging;
        std::optional<DVec3> viewDirection;
        DVec3 cameraForward{0, 0, -1};
        bool thirdManual = false;
        std::optional<SnapResult> snapped;
        std::vector<Piece> pieces;
        std::string warning;
    };
    EditorSession& session_;
    std::map<InputSource, Gesture> gestures_;
    std::optional<InputSource> current_;
    Keep keep_ = Keep::Front;
    Preview preview_;
    std::vector<std::pair<NodeId, Handle<document::Solid>>> selectedSolids() const;
    std::vector<Source> clippableSources(std::string& warning) const;
    void inferThird(Gesture& gesture, std::span<const Source> sources) const;
    SnapResult snap(const InputEvent& event, const ToolView& view) const;
    void rebuild(Gesture& gesture);
    void updatePreview();
    void publish();
    void stepBack(InputSource source);
    bool commit(InputSource source);
};

} // namespace apex::editor
