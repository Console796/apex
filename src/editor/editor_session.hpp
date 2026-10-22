#pragma once

#include "editor/actions.hpp"
#include "editor/clipboard.hpp"
#include "editor/modal_transform.hpp"
#include "editor/preview.hpp"
#include "editor/selection_ops.hpp"
#include "editor/visgroups.hpp"
#include "editor/work_plane.hpp"

#include <memory>

namespace apex::editor {

// One document's editor state. The attached document must outlive the session or
// detach(). Selection-only gestures never write to Document or its undo history.
class EditorSession {
public:
    EditorSession();
    ~EditorSession();
    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    void attach(document::Document& document);
    void detach();
    document::Document* document() const { return document_; }
    Selection& selection() { return selection_; }
    const Selection& selection() const { return selection_; }
    const PickIndex& picks() const { return *picks_; }
    const GroupIndex& groups() const { return *groups_; }
    const CycleState& cycle() const { return cycle_; }
    const ViewState& viewState() const { return view_; }
    void setViewState(ViewState view);
    std::optional<Target> hoverTarget() const { return hover_; }
    std::optional<NodeId> hoverDrawable() const { return hoverDrawable_; }
    void clearHover();
    void pointerHover(Ray ray);
    const WorkPlaneState& workPlane() const { return workPlane_; }
    std::optional<WorkPlaneState::Resolved> resolvedWorkPlane() const { return workPlane_.resolve(); }
    void pinWorkPlane();
    void pinAxisWorkPlane(Axis axis, DVec3 through);
    void unpinWorkPlane();
    double gridSize() const { return gridSize_; }
    void setGridSize(double units);
    void pointerClick(Ray ray, Modifiers modifiers = {}, bool sameSpot = false);
    void marquee(std::span<const Plane> planes, Modifiers modifiers = {});
    void selectNode(NodeId node, Modifiers modifiers = {});
    MarqueeMode marqueeMode() const { return marqueeMode_; }
    void setMarqueeMode(MarqueeMode mode);

    // Transform tooling. Drags and modal sessions publish a transient preview() and write to
    // the document only on commit, as one transaction labelled Move/Rotate/Scale. Mutating
    // tooling state is ignored while a transform is active.
    GizmoMode gizmoMode() const { return gizmoMode_; }
    void setGizmoMode(GizmoMode mode);
    GizmoSpace gizmoSpace() const { return gizmoSpace_; }
    void setGizmoSpace(GizmoSpace space);
    PivotMode pivotMode() const { return pivotMode_; }
    void setPivotMode(PivotMode mode);
    DVec3 customPivot() const { return customPivot_; }
    void setCustomPivot(DVec3 pivot);
    // gridSize follows gridSize(); fine/suspended are per-event TransformModifiers.
    const SnapSettings& snapSettings() const { return snap_; }
    void setSnapMode(SnapMode mode);
    bool textureLock() const { return textureLock_; }
    void setTextureLock(bool lock);
    // Work plane used by nudge when nothing is hovered or pinned (the viewport supplies it).
    void setFallbackPlane(SurfacePlane plane) { fallbackPlane_ = std::move(plane); }
    const SurfacePlane& fallbackPlane() const { return fallbackPlane_; }
    const Preview& preview() const { return preview_; }
    // Null while nothing transformable is selected (empty, or face mode).
    std::optional<GizmoContext> gizmoContext() const;
    bool transformActive() const { return dragging_ || modalActive_; }
    // Gizmo drag: begin on a handle, update on pointer motion, end to commit.
    bool beginGizmoDrag(GizmoHandle handle, const GizmoView& view, Ray ray);
    bool updateGizmoDrag(Ray ray, TransformModifiers modifiers = {});
    bool endGizmoDrag();
    // Modal transform (G/R/S): keys are ModalTransform names; Commit/Cancel are applied here.
    bool beginModal(GizmoMode mode, const GizmoView& view, Ray ray, DVec2 screenAnchor = DVec2{0.0});
    bool updateModal(Ray ray, TransformModifiers modifiers = {});
    ModalEvent modalKey(std::string_view key, bool shift = false);
    const ModalTransform& modal() const { return modal_; }
    const TransformReadout& activeReadout() const;
    // Aborts any active drag/modal: clears the preview, leaves document and undo untouched.
    void cancelTransform();
    // Transform dialog backend: exact values bypass snapping; one transaction each, false
    // when nothing is selected or the result is invalid (the document is then untouched).
    // Relative moves are world deltas; absolute moves put the pivot at value.
    bool moveSelection(DVec3 value, bool relative);
    // Degrees about the global X, then Y, then Z axes through the pivot.
    bool rotateSelection(DVec3 anglesDeg);
    // Per-axis factors in the current gizmo frame about the pivot.
    bool scaleSelection(DVec3 factors);
    bool flipSelection(Axis axis);
    bool nudgeSelection(bool alongV, int direction);

    // Clipboard, duplicate, delete and groups (0.2.6). Each mutating call is one undoable
    // transaction that also records the selection before and after. On failure (empty
    // selection, bad clipboard text, an invalid transform) the document is left untouched.
    // The system clipboard belongs to the UI: copyText()/cutText() produce the VMF fragment
    // and pasteText() consumes one, so edit.copy/cut/paste* are not handled by perform().
    // Paste fills unset options from the session (work plane, snap settings, texture lock)
    // and selects the pasted objects.
    Result<std::string> copyText() const;
    Result<std::string> cutText();
    Result<void> pasteText(std::string_view text, PasteOptions options = {});
    Result<void> duplicateSelection();
    Result<void> deleteSelection();
    Result<void> groupSelection();
    Result<void> ungroupSelection();
    Result<void> tieToEntity(const TieParams& params = {});
    Result<void> moveToWorld();
    Result<void> moveNodesInto(std::vector<NodeId> nodes, NodeId container);
    Result<void> renameEntity(NodeId entity, std::string_view targetname);
    Result<void> setNodeHidden(std::vector<NodeId> nodes, bool hidden);
    bool nodeHidden(NodeId node) const;
    Result<void> hideSelection();
    Result<void> unhideAll();
    Result<void> hideUnselected();
    bool toggleIsolate();
    Result<int> createVisgroup(std::string name, std::optional<int> parent = {});
    Result<void> renameVisgroup(int id, std::string name);
    Result<void> moveVisgroup(int id, std::optional<int> parent = {},
                              std::size_t position = static_cast<std::size_t>(-1));
    Result<void> deleteVisgroup(int id);
    Result<void> addSelectionToVisgroup(int id);
    Result<void> removeSelectionFromVisgroup(int id);
    Result<void> selectVisgroupMembers(int id);
    Result<void> selectAutoMembers(std::string_view id);
    Result<void> setVisgroupShown(int id, bool shown);
    Result<void> setAutoVisgroupShown(std::string_view id, bool shown);

    bool autoVisgroupShown(std::string_view id) const { return !visgroupVisibility_.automatic.contains(id); }
    bool jumpHistory(std::size_t undoCount);

    const Preset& preset() const { return preset_; }
    const Keymap& keymap() const { return keymap_; }
    void setPreset(const Preset& preset);
    bool perform(const ActionId& action);
    // Document edits go through this seam to capture before/after selection metadata.
    // Direct Document edits still update indices and prune dead selection targets.
    void edit(std::string label, const std::function<void(document::Transaction&)>& operation);

    using Observer = std::function<void()>;
    std::size_t observe(Observer observer);
    void unobserve(std::size_t token);

private:
    document::Document* document_ = nullptr;
    std::size_t documentObserver_ = 0;
    std::unique_ptr<PickIndex> picks_;
    std::unique_ptr<GroupIndex> groups_;
    Selection selection_;
    CycleState cycle_;
    ViewState view_;
    std::optional<Target> hover_;
    std::optional<NodeId> hoverDrawable_;
    std::optional<std::pair<NodeId, SideId>> hoverFace_;
    WorkPlaneState workPlane_;
    double gridSize_ = GridLadder::kDefault;
    MarqueeMode marqueeMode_ = MarqueeMode::Enclosed;
    GizmoMode gizmoMode_ = GizmoMode::Translate;
    GizmoSpace gizmoSpace_ = GizmoSpace::Global;
    PivotMode pivotMode_ = PivotMode::BoundsCentre;
    DVec3 customPivot_{0.0};
    SnapSettings snap_;
    bool textureLock_ = true;
    SurfacePlane fallbackPlane_{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {}};
    Preview preview_;
    GizmoDrag drag_;
    ModalTransform modal_;
    bool dragging_ = false, modalActive_ = false;
    GizmoMode activeMode_ = GizmoMode::Translate;
    std::vector<NodeId> activeNodes_;
    Preset preset_;
    Keymap keymap_;
    std::map<std::size_t, Observer> observers_;
    std::size_t nextObserver_ = 1;
    void notify();
    VisgroupVisibility visgroupVisibility_;
    std::map<std::uint64_t, VisgroupVisibility> visgroupHistory_;
    Result<void> runVisgroupEdit(std::string label,
        const std::function<Result<void>(document::Transaction&, VisgroupVisibility&)>& stage,
        bool pruneHidden = false);
    enum class AfterEdit { Computed, Keep, Clear, Visible };
    Result<void> runEdit(std::string label, AfterEdit after,
                         const std::function<Result<EditedObjects>(document::Transaction&)>& stage);
    std::vector<NodeId> transformNodes() const;
    void publishPreview(const Transform& transform);
    bool commitActive(const Transform& transform, GizmoMode mode);
    bool commitNow(const Transform& transform, std::string label);
    void endTransform();
};

} // namespace apex::editor
