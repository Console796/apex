#pragma once

#include "editor/modal_transform.hpp"
#include "editor/snapper.hpp"
#include "editor/tool.hpp"
#include "editor/vertex_edit.hpp"

namespace apex::editor {

enum class ElementMode { Vertex, Edge, Face };
enum class ElementKind { Vertex, Edge, Face };
struct ElementHandle {
    NodeId solid = 0;
    ElementKind kind = ElementKind::Vertex;
    std::size_t a = 0, b = 0;
    SideId side = 0;
    auto operator<=>(const ElementHandle&) const = default;
};

class VertexTool final : public Tool {
public:
    explicit VertexTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.vertex"; }
    Handled input(const InputEvent& event, const ToolView& view) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView& view, InputSource source) const override;
    ToolReadout readout() const override;
    bool perform(const ActionId& action) override;
    void cancel() override;
    void cancelTransform() override;
    void endTransform() override;
    void documentChanged() override;
    bool transformActive() const override { return moving_ || modalActive_ || awaitingMerge_; }
    void forget(InputSource source) override;
    bool owns(InputSource source) const override;
    std::optional<GizmoHandle> hovered(InputSource source) const override;
    std::optional<GizmoHandle> dragged(InputSource source) const override;

    ElementMode elementMode() const { return mode_; }
    const std::vector<ElementHandle>& elements() const { return elements_; }
    std::optional<GizmoContext> gizmoContext() const;
    bool beginGizmoDrag(GizmoHandle handle, const GizmoView& view, Ray ray);
    bool updateGizmoDrag(Ray ray, TransformModifiers modifiers = {});
    bool endGizmoDrag();
private:
    struct Source {
        Handle<document::Solid> handle;
        NodeId node = 0;
        const kv1::Node* block = nullptr;
        geometry::ConvexPolyhedron poly;
        std::vector<std::size_t> vertices;
        VertexEditResult result;
    };
    struct Pane {
        std::optional<GizmoHandle> hovered, dragged;
        std::optional<DVec2> marqueeStart;
        DVec2 cursor{0.0};
        bool marqueeAdd = false, marqueeToggle = false;
    };
    EditorSession& session_;
    ElementMode mode_ = ElementMode::Vertex;
    std::vector<ElementHandle> elements_;
    std::map<InputSource, Pane> panes_;
    std::optional<InputSource> owner_;
    std::map<NodeId, Source> sources_;
    Preview preview_;
    GizmoDrag drag_;
    ModalTransform modal_;
    bool moving_ = false, modalActive_ = false, awaitingMerge_ = false, committing_ = false, previewValid_ = false;
    GizmoMode activeMode_ = GizmoMode::Translate;
    GizmoHandle activeHandle_ = GizmoHandle::Screen;
    DVec3 snapAnchor_{0.0};
    std::uint64_t selectionRevision_ = 0;
    std::string status_;
    std::size_t absorbedCount_ = 0, mergeCount_ = 0, splitCount_ = 0;

    std::map<NodeId, geometry::SolidGeometry> selectedSolids() const;
    std::optional<ElementHandle> hitElement(const InputEvent& event, const ToolView& view) const;
    std::optional<DVec3> elementPoint(const ElementHandle& element,
                                      const std::map<NodeId, geometry::SolidGeometry>& solids) const;
    bool selectElement(ElementHandle element, bool add, bool toggle);
    bool beginMove(GizmoMode mode, const GizmoView& view, Ray ray, GizmoHandle handle,
                   std::optional<DVec3> anchor = {});
    bool beginModal(GizmoMode mode, const ToolView& view, const InputEvent& event);
    bool updateMove(Ray ray, const ToolView& view, TransformModifiers modifiers,
                    const InputEvent* event = nullptr);
    bool publish(const Transform& transform, const ToolView* view = nullptr,
                 const InputEvent* event = nullptr);
    bool commit();
    void clearMove();
    std::vector<FeatureRef> excludedFeatures() const;
    void updateSelectionAfterCommit(const std::map<NodeId, VertexEditResult>& results);
};

} // namespace apex::editor
