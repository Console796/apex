#pragma once

#include "editor/tool.hpp"
#include "editor/modal_transform.hpp"
#include "editor/light_edit.hpp"

namespace apex::editor {

class SelectTool final : public Tool {
public:
    explicit SelectTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.select"; }
    Handled input(const InputEvent& event, const ToolView& view) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView& view, InputSource source) const override;
    ToolReadout readout() const override;
    void cancel() override;
    bool setParameter(std::string_view key, double value) override;
    void pointerHover(Ray ray) override;
    void pointerClick(Ray ray, Modifiers modifiers, bool sameSpot) override;
    void marquee(std::span<const Plane> planes, Modifiers modifiers) override;
    bool owns(InputSource source) const override;
    void forget(InputSource source) override;
    std::optional<GizmoHandle> hovered(InputSource source) const override;
    std::optional<GizmoHandle> dragged(InputSource source) const override;

    std::optional<GizmoContext> gizmoContext() const;
    bool transformActive() const override { return dragging_ || modalActive_ || lightEdit_.active(); }
    bool beginGizmoDrag(GizmoHandle handle, const GizmoView& view, Ray ray);
    bool updateGizmoDrag(Ray ray, TransformModifiers modifiers);
    bool endGizmoDrag();
    bool beginModal(GizmoMode mode, const GizmoView& view, Ray ray, DVec2 screenAnchor);
    bool updateModal(Ray ray, TransformModifiers modifiers);
    ModalEvent modalKey(std::string_view key, bool shift = false);
    const ModalTransform& modal() const { return modal_; }
    const TransformReadout& activeReadout() const;
    void cancelTransform() override;
    void endTransform() override;
private:
    friend class EditorSession;
    CycleState cycle_;
    EditorSession& session_;
    SelectionGesture selectionGesture_;
    Preview preview_;
    LightEdit lightEdit_;
    GizmoDrag drag_;
    ModalTransform modal_;
    bool dragging_ = false, modalActive_ = false;
    GizmoMode activeMode_ = GizmoMode::Translate;
    std::vector<NodeId> activeNodes_;
    struct Pane {
        std::optional<GizmoHandle> hovered, dragged;
    };
    std::map<InputSource, Pane> panes_;
    std::optional<InputSource> owner_;
    void publishPreview(const Transform& transform);
    bool commitActive(const Transform& transform, GizmoMode mode);
    std::vector<NodeId> transformNodes() const;
    bool transformInput(const InputEvent& event, const ToolView& view);
};

} // namespace apex::editor
