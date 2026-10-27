#pragma once

#include "editor/instance_expansion.hpp"

#include "editor/actions.hpp"
#include "editor/name_index.hpp"
#include "editor/face_edit.hpp"
#include "editor/topology_ops.hpp"
#include "editor/tool.hpp"
#include "editor/clipboard.hpp"
#include "editor/inspector_model.hpp"
#include "editor/connection_model.hpp"
#include "editor/rename_propagation.hpp"
#include "editor/placement.hpp"
#include "editor/modal_transform.hpp"
#include "editor/overlay_palette.hpp"
#include "editor/preview.hpp"
#include "editor/selection_ops.hpp"
#include "editor/pie_menu.hpp"
#include "editor/visgroups.hpp"
#include "editor/work_plane.hpp"
#include "editor/snapper.hpp"
#include "geometry/solid_block.hpp"

#include <memory>
#include <span>
#include <chrono>
#include <filesystem>
#include <functional>

namespace apex::editor {

struct InstanceCollapseItemResult {
    NodeId instance = 0;
    bool collapsed = false;
    std::string reason;
};
using InstanceCollapseReport = std::vector<InstanceCollapseItemResult>;

class ConnectionValidator;
class InstanceTemplates;

class SelectTool;
class EntityTool;
class VertexTool;

// One document's editor state. The attached document must outlive the session or
// detach(). Selection-only gestures never write to Document or its undo history.
class EditorSession {
public:
    // 0.2 compatibility spellings; tool identity is now the registry action id.
    struct Tool {
        static constexpr const char* Select = "tool.select";
        static constexpr const char* Entity = "tool.entity";
        static constexpr const char* Block = "tool.block";
        static constexpr const char* Clip = "tool.clip";
        static constexpr const char* Primitive = "tool.primitive";
        static constexpr const char* Vertex = "tool.vertex";
        static constexpr const char* Apply = "tool.apply";
        static constexpr const char* Measure = "tool.measure";
    };
    const ToolId& activeTool() const { return activeTool_; }
    void setActiveTool(ToolId tool);
    editor::Tool& activeToolObject();
    const editor::Tool& activeToolObject() const;
    bool input(const InputEvent& event, const ToolView& view);
    void setClock(std::function<std::chrono::milliseconds()> clock) { clock_ = std::move(clock); }
    const PieSettings& pieSettings() const { return pieSettings_; }
    std::chrono::milliseconds pieTime() const { return clock_(); }
    void setPieSettings(PieSettings settings) { pieSettings_ = settings; }
    using PieTrace = std::function<void(std::string_view, std::string_view, std::chrono::milliseconds)>;
    void setPieTrace(PieTrace trace) { pieTrace_ = std::move(trace); }
    void tickPie();
    void pieFocusLost();
    const PieGesture* pie() const { return pie_.get(); }
    std::optional<InputSource> pieSource() const { return pieSource_; }
    const std::string& pieRefusal() const { return pieRefusal_; }
    // Every tool drops the per-pane state of a pane that is going away.
    void forgetInputSource(InputSource source);
    const std::string& currentEntityClass() const { return currentEntityClass_; }
    void setCurrentEntityClass(std::string classname);
    const std::string& placementModel() const { return placementModel_; }
    void setPlacementModel(std::string model);
    const std::string& modelPlacementClass() const { return modelPlacementClass_; }
    void setModelPlacementClass(std::string classname);
    bool alignToSurface() const { return alignToSurface_; }
    void setAlignToSurface(bool align);

    const geometry::NewSolidStyle& newSolidStyle() const { return newSolidStyle_; }
    void setNewSolidMaterial(std::string material);
    void setNewSolidUvAlignment(geometry::UvAlignment alignment);
    FaceExtrapolation faceExtrapolation() const { return faceExtrapolation_; }
    void setFaceExtrapolation(FaceExtrapolation mode);
    void cycleFaceExtrapolation();

    EditorSession();
    ~EditorSession();
    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    void attach(document::Document& document);
    void setEntityBoundsProvider(EntityBoundsProvider provider);
    void refreshEntityBounds();
    void detach();
    document::Document* document() const { return document_; }
    Selection& selection() { return selection_; }
    const Selection& selection() const { return selection_; }
    // Bounds of the expanded selection (group members, brush children, selected faces).
    std::optional<AABB> selectionBounds() const;
    const PickIndex& picks() const { return *picks_; }
    const GroupIndex& groups() const { return *groups_; }
    const NameIndex& names() const { return *names_; }
    ConnectionValidator connectionValidator() const;
    void setConnectionContext(std::filesystem::path map,
                              std::optional<std::filesystem::path> extraRoot = {});
    const InstanceExpansion& instances() const { return *instanceExpansion_; }
    InstanceReload invalidateInstance(const std::filesystem::path& path);
    document::InstanceVisibility instanceVisibility() const;
    void setInstanceVisibility(document::InstanceVisibility visibility);
    const CycleState& cycle() const;
    const ViewState& viewState() const { return view_; }
    void setViewState(ViewState view);
    std::optional<Target> hoverTarget() const { return hover_; }
    std::optional<NodeId> hoverDrawable() const { return hoverDrawable_; }
    void clearHover();
    void pointerHover(Ray ray);
    const WorkPlaneState& workPlane() const { return workPlane_; }
    std::optional<WorkPlaneState::Resolved> resolvedWorkPlane() const { return workPlane_.resolve(); }
    void pinWorkPlane();
    void pinWorkPlane(const SurfacePlane& plane);
    void pinAxisWorkPlane(Axis axis, DVec3 through);
    void unpinWorkPlane();
    double gridSize() const { return gridSize_; }
    void setGridSize(double units);
    const OverlayPalette& overlayPalette() const { return overlayPalette_; }
    void setOverlayPalette(const OverlayPalette& palette);
    SnapResult snap(Ray ray, const ToolView& view, bool suspended = false,
                    std::optional<SurfacePlane> plane = {}, bool edges = true,
                    std::vector<FeatureRef> excluded = {}) const;
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
    // Hammer's behaviour: a single selected object rotates and scales about its own origin. Off pivots on the
    // selection's bounds centre. Only applies while the pivot mode is BoundsCentre.
    bool pivotAtObjectOrigin() const { return pivotAtObjectOrigin_; }
    void setPivotAtObjectOrigin(bool enabled);
    // Model triangle edges drawn over models outside the Wireframe mode: the Flat edge overlay and the
    // selection outline. Wireframe mode always draws them.
    bool modelEdgeOverlay() const { return modelEdgeOverlay_; }
    void setModelEdgeOverlay(bool enabled);
    // Work plane used by nudge when nothing is hovered or pinned (the viewport supplies it).
    void setFallbackPlane(SurfacePlane plane) { fallbackPlane_ = std::move(plane); }
    const SurfacePlane& fallbackPlane() const { return fallbackPlane_; }
    const Preview& preview() const { return topologyPreview_.empty() ? activeToolObject().preview() : topologyPreview_; }
    // Null while nothing transformable is selected (empty, or face mode).
    std::optional<GizmoContext> gizmoContext() const;
    bool transformActive() const;
    // Gizmo drag: begin on a handle, update on pointer motion, end to commit.
    bool beginGizmoDrag(GizmoHandle handle, const GizmoView& view, Ray ray);
    bool updateGizmoDrag(Ray ray, TransformModifiers modifiers = {});
    bool endGizmoDrag();
    // Modal transform (G/R/S): keys are ModalTransform names; Commit/Cancel are applied here.
    bool beginModal(GizmoMode mode, const GizmoView& view, Ray ray, DVec2 screenAnchor = DVec2{0.0});
    bool updateModal(Ray ray, TransformModifiers modifiers = {});
    ModalEvent modalKey(std::string_view key, bool shift = false);
    const ModalTransform& modal() const;
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

    void setEntityClasses(assets::EntityClassTable classes);
    const assets::EntityClassTable& entityClasses() const { return entityClasses_; }
    std::uint64_t entityClassesRevision() const { return entityClassesRevision_; }
    ConnectionModel connectionModel() const;
    Result<void> setConnectionField(std::size_t row, ConnectionModel::Field field, std::string text);
    Result<void> setConnectionOutput(std::size_t row, std::string name);
    Result<void> addConnection(std::string output, document::Connection connection,
                               std::optional<std::size_t> after = {});
    Result<void> removeConnection(std::size_t row);
    Result<void> removeConnections(std::vector<std::size_t> rows);
    Result<void> addConnections(std::vector<std::pair<std::string, document::Connection>> connections,
                               std::optional<std::size_t> after = {});
    Result<void> duplicateConnection(std::size_t row);
    InspectorModel inspectorModel(bool raw = false) const;
    // Supply a unique nonempty token for each field focus to coalesce typing. Rebuild the
    // model after each commit; raw selects occurrence-aware rows. Empty means one undo step.
    Result<void> editProperty(std::size_t row, std::string text, std::string focusKey = {}, bool raw = false);
    Result<void> editVectorComponent(std::size_t row, std::size_t component, std::string text, std::string focusKey = {});
    Result<void> toggleFlag(std::size_t row, std::uint64_t bit, bool enabled, std::string focusKey = {});
    Result<void> setChoice(std::size_t row, std::string value, std::string focusKey = {});
    Result<void> setClass(std::string classname);
    // With no normal, hit is projected onto the resolved work plane and snapped in-plane.
    Result<void> placeEntity(std::string classname, DVec3 hit, std::optional<DVec3> normal, bool align = false);
    Result<void> placeEntity(std::string classname, Ray ray, bool align = false);
    Result<void> placeModel(std::string model, DVec3 hit, std::optional<DVec3> normal, bool align = false);
    Result<void> placeModel(std::string model, Ray ray, bool align = false);

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
    Result<InstanceCollapseReport> collapseInstances(std::span<const NodeId> selection);
    Result<InstanceCollapseReport> collapseAllInstances();
    Result<void> deleteSelection();
    Result<void> groupSelection();
    Result<void> ungroupSelection();
    Result<void> tieToEntity(const TieParams& params = {});
    Result<void> moveToWorld();
    Result<void> moveNodesInto(std::vector<NodeId> nodes, NodeId container);
    RenamePlan planEntityRename(NodeId entity, std::string_view targetname) const;
    Result<void> renameEntity(NodeId entity, std::string_view targetname);
    Result<void> renameEntity(NodeId entity, std::string_view targetname, bool updateReferences);
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
    bool isToolEnabled(const ActionId& action) const;
    bool perform(const ActionId& action);
    bool confirmNonConvexMerge = true;
    // The host sees preview() populated during the callback. Missing host means Cancel.
    std::function<bool(const MergePlan&)> confirmMerge;
    std::function<std::optional<double>()> hollowThickness;
    const std::string& topologyMessage() const { return topologyMessage_; }
    Result<void> mergeSelection();
    // One "Apply Material" edit over `sides`, skipping sides that already have it (any case).
    // Returns how many changed; zero leaves no undo entry.
    std::size_t applyMaterial(std::span<const document::SideId> sides, const std::string& material);
    // Applies to the face `ray` hits first (a material drop); false when no face is hit.
    bool applyMaterialAt(Ray ray, const std::string& material);
    // Face edits use Faces selection only. Rotation is an absolute angle derived from axes.
    Result<std::size_t> setFaceField(FaceField field, FaceValue value, std::string token = {});
    Result<std::size_t> justifyFaces(geometry::Justify mode, bool treatAsOne = false);
    Result<std::size_t> justifyFaces(geometry::HorizontalAlignment horizontal,
                                     geometry::VerticalAlignment vertical, bool treatAsOne = false);
    Result<std::size_t> editFaceGesture(FaceGesture gesture, double u, double v, DVec3 activePivot,
                                        std::string token = {});
    Result<std::size_t> alignFaces(FaceAlignment alignment);
    Result<std::size_t> rotateFaces(double degrees, std::string token = {});
    Result<std::size_t> toggleFaceSmoothingGroup(std::uint32_t bit, bool enabled);
    Result<FaceApplyResult> applyMaterialWrapped(Ray ray);
    bool liftMaterialAt(Ray ray, bool alignSource = false);
    // Host supplies actual dimensions; unknown materials use the editor's 256x256 fallback.
    std::function<std::pair<int, int>(std::string_view)> faceTextureSize =
        [](std::string_view) { return std::pair{256, 256}; };
    // Document edits go through this seam to capture before/after selection metadata.
    // Direct Document edits still update indices and prune dead selection targets. A nonempty
    // coalesceKey merges with the previous edit of the same key (typing); empty is its own undo step.
    void edit(std::string label, const std::function<void(document::Transaction&)>& operation,
              std::string coalesceKey = {});

    using Observer = std::function<void()>;
    std::size_t observe(Observer observer);
    void unobserve(std::size_t token);
    // A tool calls this after changing transient data exposed through readout().
    void publishToolStatus() { notify(); }

private:
    friend Result<InstanceCollapseReport> stageInstanceCollapse(EditorSession&, document::Document&,
        std::span<const NodeId>, bool);
    void finishPie(PieResult result);
    Preview topologyPreview_;
    std::string topologyMessage_;
    friend class SelectTool;
    friend class DrawExtrudeTool;
    friend class PrimitiveTool;
    friend class ClipTool;
    friend class MeasureTool;
    friend class VertexTool;
    SelectTool& selectTool();
    const SelectTool& selectTool() const;
    document::Document* document_ = nullptr;
    std::size_t documentObserver_ = 0;
    std::unique_ptr<PickIndex> picks_;
    EntityBoundsProvider entityBoundsProvider_;
    std::unique_ptr<GroupIndex> groups_;
    std::unique_ptr<NameIndex> names_;
    std::filesystem::path connectionMapPath_;
    std::optional<std::filesystem::path> connectionExtraRoot_;
    mutable std::unique_ptr<InstanceTemplates> instanceTemplates_;
    std::unique_ptr<InstanceExpansion> instanceExpansion_;
    void refreshInstances();
    Selection selection_;
    std::map<ToolId, std::unique_ptr<editor::Tool>> tools_;
    ToolId activeTool_ = Tool::Select;
    std::string currentEntityClass_;
    std::string placementModel_;
    std::string modelPlacementClass_ = "prop_static";
    struct PlacementTarget { DVec3 point; std::optional<DVec3> normal; };
    Result<PlacementTarget> placementTarget(Ray ray) const;
    Result<EntityPlacement> surfacePlacement(DVec3 hit, std::optional<DVec3> normal,
                                            const AABB& bounds, bool align) const;
    Result<void> placePointEntity(std::string classname, std::optional<std::string> model,
                                 DVec3 hit, std::optional<DVec3> normal, bool align);
    bool alignToSurface_ = false;
    geometry::NewSolidStyle newSolidStyle_{"dev/dev_measuregeneric01"};
    FaceExtrapolation faceExtrapolation_ = FaceExtrapolation::Wrap;
    assets::EntityClassTable entityClasses_;
    std::uint64_t entityClassesRevision_ = 0;
    ViewState view_;
    std::optional<Target> hover_;
    std::optional<NodeId> hoverDrawable_;
    std::optional<std::pair<NodeId, SideId>> hoverFace_;
    WorkPlaneState workPlane_;
    OverlayPalette overlayPalette_;
    double gridSize_ = GridLadder::kDefault;
    MarqueeMode marqueeMode_ = MarqueeMode::Enclosed;
    GizmoMode gizmoMode_ = GizmoMode::Translate;
    GizmoSpace gizmoSpace_ = GizmoSpace::Global;
    PivotMode pivotMode_ = PivotMode::BoundsCentre;
    DVec3 customPivot_{0.0};
    SnapSettings snap_;
    bool textureLock_ = true;
    bool pivotAtObjectOrigin_ = true;
    bool modelEdgeOverlay_ = true;
    SurfacePlane fallbackPlane_{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {}};
    Preset preset_;
    Keymap keymap_;
    std::map<std::size_t, Observer> observers_;
    std::size_t nextObserver_ = 1;
    PieTrace pieTrace_;
    PieSettings pieSettings_;
    std::optional<KeyChord> pieRelease_;
    std::unique_ptr<PieGesture> pie_;
    std::optional<InputSource> pieSource_;
    std::string pieRefusal_;
    std::function<std::chrono::milliseconds()> clock_;
    void notify();
    VisgroupVisibility visgroupVisibility_;
    std::map<std::uint64_t, VisgroupVisibility> visgroupHistory_;
    Result<void> runVisgroupEdit(std::string label,
        const std::function<Result<void>(document::Transaction&, VisgroupVisibility&)>& stage,
        bool pruneHidden = false);
    enum class AfterEdit { Computed, Keep, Clear, Visible };
    Result<void> runEdit(std::string label, AfterEdit after,
                         const std::function<Result<EditedObjects>(document::Transaction&)>& stage,
                         std::string coalesceKey = {});
    std::string inspectorCoalesceKey(std::size_t row, std::string_view focusKey, bool raw = false) const;
    bool commitNow(const Transform& transform, std::string label);
    void endTransform();
};

} // namespace apex::editor
