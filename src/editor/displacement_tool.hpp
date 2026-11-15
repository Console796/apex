#pragma once

// Plan 0.8 D6: the Displacement tool. Its readout picks a mode: Create (power and elevation),
// Sew, Sculpt or Paint alpha. Subdivide was dropped (2026-11-14): Sculpt's Smooth brush covers it.
// In Create and Sew a click selects faces as in the face edit tool (Ctrl toggles, Shift adds);
// Enter, or the "displacement.apply" action, applies the mode to the selected faces as one undo
// step. The edits are in displacement_edit.hpp.
//
// Sculpt and Paint alpha (0.8.4) act by dragging over displacements in any viewport: the stroke
// runs on a tool-owned copy (geometry::DisplacementStroke), drawn through the preview as it goes,
// and is committed at release as one labelled undo step with one setDisplacement per touched side.
// When displaced faces are selected only they are brushed, otherwise every visible displacement;
// vertices shared with a sewn neighbour move on both sides either way.

#include "editor/tool.hpp"
#include "geometry/displacement_sculpt.hpp"

#include <map>
#include <optional>
#include <vector>

namespace apex::editor {

class DisplacementTool final : public Tool {
public:
    enum class Mode { Create, Sew, Sculpt, PaintAlpha };
    explicit DisplacementTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.displacement"; }
    void activate() override;
    Handled input(const InputEvent& event, const ToolView& view) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView& view, InputSource source) const override;
    ToolReadout readout() const override;
    // Drops a stroke in progress, leaving the document as it was.
    void cancel() override;
    // A stroke is the tool's gesture: Escape (selection.none) cancels it, and the tools pie waits.
    bool transformActive() const override { return stroking_; }
    bool owns(InputSource source) const override { return stroking_ && source == strokeSource_; }
    void forget(InputSource source) override {
        if (owns(source)) cancel();
    }
    bool setParameter(std::string_view id, double value) override;
    bool perform(const ActionId& action) override;

    Mode mode() const { return mode_; }
    void setMode(Mode mode);
    int power() const { return power_; }
    double elevation() const { return elevation_; }
    // Applies the mode to the selected faces; false (with the reason in the readout) when it did nothing.
    bool apply();
    const std::string& notice() const { return notice_; }
    bool stroking() const { return stroking_; }
    // The brush a Sculpt stroke uses, and a Paint alpha stroke's (radius, falloff and strength are shared).
    geometry::SculptSettings sculptSettings() const;
    geometry::PaintSettings paintSettings() const;

    // Brush spacing along a drag: a dab every quarter radius (at least a unit), so a stroke's
    // result does not depend on how often the mouse reports.
    static double dabSpacing(double radius);

private:
    struct Surface {
        document::SideId side = 0;
        NodeId solid = 0;
    };
    void setNotice(std::string notice);
    // The displacements a stroke can reach, read once per document state.
    bool prepare();
    Handled strokeInput(const InputEvent& event, const ToolView& view);
    void dab(DVec3 centre);
    void publishPreview();
    void commit();

    EditorSession& session_;
    Preview preview_;
    Mode mode_ = Mode::Create;
    int power_ = 3;
    double elevation_ = 0.0;
    std::string notice_;

    // Sculpt and Paint alpha.
    geometry::SculptBrush brush_ = geometry::SculptBrush::RaiseLower;
    geometry::BrushFalloff falloff_ = geometry::BrushFalloff::Smooth;
    geometry::SculptDirection direction_ = geometry::SculptDirection::FaceNormal;
    double radius_ = 64.0, amount_ = 2.0, strength_ = 0.25, height_ = 0.0, alpha_ = 255.0;
    int seed_ = 1;

    std::optional<geometry::DisplacementStroke> stroke_;
    std::vector<Surface> surfaces_; // Parallel to stroke_'s patches.
    std::map<NodeId, std::vector<std::size_t>> solidSurfaces_;
    std::map<NodeId, geometry::ConvexPolyhedron> polys_;
    const document::Document* preparedDocument_ = nullptr;
    std::uint64_t preparedState_ = 0;
    bool stroking_ = false, invert_ = false;
    InputSource strokeSource_ = 0;
    DVec3 last_{0.0};
    double carry_ = 0.0; // Distance dragged since the last dab.
    std::optional<geometry::DisplacementStroke::Hit> cursor_;
};

} // namespace apex::editor
