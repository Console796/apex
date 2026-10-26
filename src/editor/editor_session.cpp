#include "editor/editor_session.hpp"
#include "editor/pie_menus.hpp"

#include "core/strings.hpp"
#include "editor/select_tool.hpp"
#include "editor/entity_tool.hpp"
#include "editor/block_tool.hpp"
#include "editor/primitive_tool.hpp"
#include "editor/clip_tool.hpp"
#include "editor/apply_tool.hpp"
#include "editor/measure_tool.hpp"
#include "editor/vertex_tool.hpp"
#include "editor/hide_ops.hpp"
#include "editor/selection_expand.hpp"
#include "editor/selection_undo.hpp"
#include "editor/visibility.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace apex::editor {

EditorSession::EditorSession() : preset_(hammerPreset()), keymap_(preset_) {
    clock_ = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
    };
    tools_.emplace(Tool::Select, std::make_unique<SelectTool>(*this));
    tools_.emplace(Tool::Entity, std::make_unique<EntityTool>(*this));
    tools_.emplace(Tool::Block, std::make_unique<BlockTool>(*this));
    tools_.emplace(Tool::Primitive, std::make_unique<PrimitiveTool>(*this));
    tools_.emplace(Tool::Clip, std::make_unique<ClipTool>(*this));
    tools_.emplace(Tool::Vertex, std::make_unique<VertexTool>(*this));
    tools_.emplace(Tool::Apply, std::make_unique<ApplyTool>(*this));
    tools_.emplace(Tool::Measure, std::make_unique<MeasureTool>(*this));
    activeToolObject().activate();
    selection_.observe([this](const Selection&) { notify(); });
}
EditorSession::~EditorSession() { detach(); }

void EditorSession::attach(document::Document& doc) {
    detach();
    document_ = &doc;
    visgroupVisibility_ = buildVisgroupVisibility(doc);
    visgroupHistory_[doc.historyState()] = visgroupVisibility_;
    picks_ = std::make_unique<PickIndex>(doc, [this](const kv1::Node& block) -> std::optional<EntityPickShape> {
        if (entityBoundsProvider_) {
            if (auto shape = entityBoundsProvider_(block)) return shape;
        }
        const auto* key = block.find("classname");
        const auto name = key ? key->decodedValue().value_or("") : "";
        const auto* cls = entityClasses_.find(name);
        if (!cls || !cls->mins || !cls->maxs) return {};
        return EntityPickShape{{}, 0, LocalBox{{(*cls->mins)[0], (*cls->mins)[1], (*cls->mins)[2]},
                                            {(*cls->maxs)[0], (*cls->maxs)[1], (*cls->maxs)[2]}}};
    });
    groups_ = std::make_unique<GroupIndex>(doc);
    names_ = std::make_unique<NameIndex>(doc);
    documentObserver_ = doc.observe([this](const document::ChangeSet& changes) {
        const auto count = document_->historyState();
        if (visgroupHistory_.contains(count))
            visgroupVisibility_ = visgroupHistory_.at(count);
        else {
            if (count < visgroupHistory_.rbegin()->first)
                visgroupVisibility_ = buildVisgroupVisibility(*document_);
            visgroupHistory_[count] = visgroupVisibility_;
        }
        picks_->apply(changes);
        groups_->apply(changes);
        names_->apply(changes);
        activeToolObject().documentChanged();
        if (transformActive()) endTransform(); // Undo/redo/external edits invalidate a drag.
        selectTool().cycle_ = {};
        hover_.reset();
        hoverDrawable_.reset();
        hoverFace_.reset();
        workPlane_.setHovered(std::nullopt);
        selection_.prune(*document_);
        notify();
    });
    notify();
}
void EditorSession::setEntityBoundsProvider(EntityBoundsProvider provider) {
    entityBoundsProvider_ = std::move(provider);
    refreshEntityBounds();
}
void EditorSession::refreshEntityBounds() {
    if (!picks_) return;
    picks_->rebuild();
    notify();
}
void EditorSession::detach() {
    if (transformActive()) endTransform();
    activeToolObject().cancel();
    if (document_) document_->unobserve(documentObserver_);
    document_ = nullptr;
    visgroupHistory_.clear();
    visgroupVisibility_ = {};
    picks_.reset();
    groups_.reset();
    names_.reset();
    selectTool().cycle_ = {};
    view_ = {};
    hover_.reset();
    hoverDrawable_.reset();
    hoverFace_.reset();
    workPlane_ = {};
    selection_.clear();
    notify();
}
void EditorSession::setViewState(ViewState view) {
    view_ = std::move(view);
    if (picks_) picks_->setViewState(view_);
    selectTool().cycle_ = {};
    hover_.reset();
    hoverDrawable_.reset();
    hoverFace_.reset();
    workPlane_.setHovered(std::nullopt);
    notify();
}
void EditorSession::clearHover() {
    if (!hover_ && !hoverDrawable_ && !hoverFace_) return;
    hover_.reset();
    hoverDrawable_.reset();
    hoverFace_.reset();
    workPlane_.setHovered(std::nullopt);
    notify();
}

void EditorSession::pinWorkPlane() {
    if (!hoverFace_) return;
    workPlane_.pinHovered();
    notify();
}
void EditorSession::pinWorkPlane(const SurfacePlane& plane) {
    workPlane_.pin(plane);
    notify();
}
void EditorSession::pinAxisWorkPlane(Axis axis, DVec3 through) {
    workPlane_.pinAxisPlane(axis, through);
    notify();
}
void EditorSession::unpinWorkPlane() {
    if (!workPlane_.pinned()) return;
    workPlane_.unpin();
    notify();
}
SnapResult EditorSession::snap(Ray ray, const ToolView& view, bool suspended,
                               std::optional<SurfacePlane> plane, bool edges,
                               std::vector<FeatureRef> excluded) const {
    if (!document_ || !picks_) return {};
    auto settings = snap_;
    settings.gridSize = gridSize_;
    return Snapper(*document_, *picks_, workPlane_, settings, fallbackPlane_).snap(
        ray, view, {suspended, std::move(plane), edges, std::move(excluded)});
}

void EditorSession::setOverlayPalette(const OverlayPalette& palette) {
    if (palette == overlayPalette_) return;
    overlayPalette_ = palette;
    notify();
}

void EditorSession::setGridSize(double units) {
    units = GridLadder::clamp(units);
    if (units == gridSize_) return;
    gridSize_ = units;
    notify();
}

void EditorSession::selectNode(NodeId node, Modifiers modifiers) {
    if (!document_ || !document_->handle(node)) return;
    selectTool().cycle_ = {};
    std::vector<Target> targets;
    const auto handle = *document_->handle(node);
    if (std::holds_alternative<Handle<document::Group>>(handle) &&
        selection_.mode() == SelectionMode::Groups && !selection_.ignoreGroups()) {
        targets.emplace_back(groups_->outermostGroup(node).value_or(node));
    } else {
        Selection source(SelectionMode::Objects);
        source.select(node);
        for (const auto drawable : expandSelection(*document_, *groups_, source).solidsAndEntities) {
            if (selection_.mode() == SelectionMode::Faces) {
                const auto drawableHandle = *document_->handle(drawable);
                if (!std::holds_alternative<Handle<document::Solid>>(drawableHandle)) continue;
                const auto* solid = document_->get(drawableHandle);
                for (const auto& side : solid->block->children()) {
                    if (!ciEquals(side.key(), "side")) continue;
                    if (const auto* number = side.find("id")) {
                        if (const auto id = number->asInt(); id && *id >= 0) targets.emplace_back(SideId{*id});
                    }
                }
            } else if (const auto target = resolveClick({drawable, {}, {}, {}, 0}, selection_.mode(),
                                                       *groups_, selection_.ignoreGroups())) {
                targets.push_back(*target);
            }
        }
    }
    if (!modifiers.ctrl && !modifiers.shift) selection_.replace(targets);
    else for (const auto target : targets) {
        if (modifiers.ctrl) selection_.toggle(target);
        else selection_.add(target);
    }
}
void EditorSession::setMarqueeMode(MarqueeMode mode) {
    if (marqueeMode_ == mode) return;
    marqueeMode_ = mode;
    notify();
}
void EditorSession::setPreset(const Preset& preset) {
    Keymap keymap(preset);
    preset_ = preset;
    keymap_ = std::move(keymap);
    notify();
}
void EditorSession::setActiveTool(ToolId tool) {
    if (activeTool_ == tool || !tools_.contains(tool)) return;
    activeToolObject().deactivate();
    activeTool_ = std::move(tool);
    activeToolObject().activate();
    selectTool().cycle_ = {};
    clearHover();
    notify();
}
void EditorSession::setCurrentEntityClass(std::string classname) {
    if (currentEntityClass_ == classname) return;
    currentEntityClass_ = std::move(classname);
    notify();
}
void EditorSession::setAlignToSurface(bool align) {
    if (alignToSurface_ == align) return;
    alignToSurface_ = align;
    notify();
}
void EditorSession::setNewSolidMaterial(std::string material) {
    if (newSolidStyle_.material == material) return;
    newSolidStyle_.material = std::move(material);
    notify();
}
void EditorSession::setNewSolidUvAlignment(geometry::UvAlignment alignment) {
    if (newSolidStyle_.alignment == alignment) return;
    newSolidStyle_.alignment = alignment;
    notify();
}
void EditorSession::setFaceExtrapolation(FaceExtrapolation mode) {
    if (faceExtrapolation_ == mode) return;
    faceExtrapolation_ = mode;
    notify();
}
void EditorSession::cycleFaceExtrapolation() {
    setFaceExtrapolation(faceExtrapolation_ == FaceExtrapolation::Wrap ? FaceExtrapolation::Projected
                         : faceExtrapolation_ == FaceExtrapolation::Projected ? FaceExtrapolation::Match
                         : FaceExtrapolation::Wrap);
}
std::optional<AABB> EditorSession::selectionBounds() const {
    if (!document_) return std::nullopt;
    const auto expanded = expandSelection(*document_, *groups_, selection_);
    AABB bounds;
    for (const auto node : expanded.solidsAndEntities) {
        if (const auto box = picks_->bounds(node)) bounds.expand(*box);
    }
    for (const auto side : expanded.sides) {
        if (const auto location = document_->side(side)) {
            if (const auto box = picks_->bounds(document_->get(location->solid)->id, side)) bounds.expand(*box);
        }
    }
    if (bounds.empty()) return std::nullopt;
    return bounds;
}
std::size_t EditorSession::applyMaterial(std::span<const document::SideId> sides, const std::string& material) {
    if (!document_) return 0;
    std::vector<document::SideId> changed;
    for (const auto id : sides) {
        const auto location = document_->side(id);
        const auto* solid = location ? document_->get(location->solid) : nullptr;
        if (!solid || !solid->block) continue;
        std::size_t face = 0;
        for (const auto& child : solid->block->children()) {
            if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
            if (face++ != location->faceIndex) continue;
            const auto* key = child.find("material");
            if (!key || !ciEquals(key->decodedValue().value_or(""), material)) changed.push_back(id);
            break;
        }
    }
    if (changed.empty()) return 0;
    edit("Apply Material", [&](document::Transaction& tx) {
        for (const auto id : changed) tx.setSideKeyValue(id, "material", material);
    });
    return changed.size();
}

bool EditorSession::applyMaterialAt(Ray ray, const std::string& material) {
    if (!document_ || material.empty()) return false;
    const auto hit = picks_->first(ray);
    if (!hit || !hit->side) return false;
    applyMaterial(std::span(&*hit->side, 1), material);
    return true;
}

Result<void> EditorSession::mergeSelection() {
    activeToolObject().cancel();
    const auto plan = planMerge(*this);
    if (!plan) return std::unexpected(plan.error());
    if (!plan->convex() && confirmNonConvexMerge) {
        std::vector<GhostSolid> ghosts;
        for (const auto& poly : plan->ghosts) ghosts.push_back({poly, {}, 0.35f, overlayPalette().clip});
        topologyPreview_.setGhosts(ghosts);
        notify();
        const auto* originalDocument = document_;
        const auto state = document_->historyState();
        const bool accepted = confirmMerge && confirmMerge(*plan);
        topologyPreview_.clear();
        notify();
        if (!accepted) return {};
        if (document_ != originalDocument || !document_ || document_->historyState() != state)
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Document changed during merge confirmation"});
    }
    return commitMerge(*this, *plan);
}

bool EditorSession::isToolEnabled(const ActionId& action) const {
    return tools_.contains(action) && (document_ || action == Tool::Select);
}
bool EditorSession::perform(const ActionId& action) {
    if (activeToolObject().perform(action)) return true;
    // UI entry points use this policy; perform() remains available to programmatic callers.
    if (tools_.contains(action)) {
        setActiveTool(action);
        return true;
    }
    if (action == "face.extrapolation.cycle") {
        cycleFaceExtrapolation();
        return true;
    }
    if (!document_) return false;
    if (action == "edit.carve" || action == "edit.hollow" || action == "edit.merge") {
        if (transformActive()) return false;
        activeToolObject().cancel();
        Result<void> result;
        if (action == "edit.carve") result = carve(*this);
        else if (action == "edit.merge") result = mergeSelection();
        else {
            const auto thickness = hollowThickness ? hollowThickness() : std::nullopt;
            if (!thickness) return true;
            result = hollow(*this, *thickness);
        }
        topologyMessage_ = result ? "" : result.error().what();
        notify();
        return result.has_value();
    }
    if (action == "grid.larger" || action == "grid.smaller") {
        setGridSize(action == "grid.larger" ? GridLadder::larger(gridSize_) : GridLadder::smaller(gridSize_));
        return true;
    }
    if (action == "workplane.pin") {
        pinWorkPlane();
        return true;
    }
    if (action == "workplane.axis_x" || action == "workplane.axis_y" || action == "workplane.axis_z") {
        const auto bounds = selectionBounds();
        const auto axis = action == "workplane.axis_x" ? Axis::X : action == "workplane.axis_y" ? Axis::Y : Axis::Z;
        pinAxisWorkPlane(axis, bounds ? bounds->center() : DVec3{0.0});
        return true;
    }
    // Escape: an active drag/modal is cancelled first, then the pin, then the selection.
    if (action == "selection.none" && transformActive()) {
        cancelTransform();
        return true;
    }
    if (action == "gizmo.mode.translate" || action == "gizmo.mode.rotate" || action == "gizmo.mode.scale") {
        setGizmoMode(action == "gizmo.mode.translate" ? GizmoMode::Translate
                     : action == "gizmo.mode.rotate" ? GizmoMode::Rotate : GizmoMode::Scale);
        return true;
    }
    if (action == "gizmo.space.toggle") {
        setGizmoSpace(gizmoSpace_ == GizmoSpace::Global ? GizmoSpace::Local : GizmoSpace::Global);
        return true;
    }
    if (action == "gizmo.pivot.cycle") {
        setPivotMode(pivotMode_ == PivotMode::BoundsCentre ? PivotMode::ActiveOrigin
                     : pivotMode_ == PivotMode::ActiveOrigin ? PivotMode::Custom : PivotMode::BoundsCentre);
        return true;
    }
    if (action == "snap.mode.toggle") {
        setSnapMode(snap_.mode == SnapMode::Absolute ? SnapMode::Relative : SnapMode::Absolute);
        return true;
    }
    if (action == "gizmo.pivot.origin.toggle") {
        setPivotAtObjectOrigin(!pivotAtObjectOrigin_);
        return true;
    }
    if (action == "view.model_overlay.toggle") {
        setModelEdgeOverlay(!modelEdgeOverlay_);
        return true;
    }
    if (action == "transform.texture_lock.toggle") {
        setTextureLock(!textureLock_);
        return true;
    }
    if (action == "edit.flip.x" || action == "edit.flip.y" || action == "edit.flip.z")
        return flipSelection(action == "edit.flip.x" ? Axis::X : action == "edit.flip.y" ? Axis::Y : Axis::Z);
    if (action == "nudge.left" || action == "nudge.right" || action == "nudge.up" || action == "nudge.down")
        return nudgeSelection(action == "nudge.up" || action == "nudge.down",
                              action == "nudge.right" || action == "nudge.up" ? 1 : -1);
    // Modal input is dispatched by the active tool; the dialog and held snap modifier
    // use the dialog backend and TransformModifiers.
    if (action == "selection.none" && workPlane_.pinned()) {
        unpinWorkPlane();
        return true;
    }
    if (action == "edit.delete") return deleteSelection().has_value();
    if (action == "edit.duplicate") return duplicateSelection().has_value();
    if (action == "edit.group") return groupSelection().has_value();
    if (action == "edit.ungroup") return ungroupSelection().has_value();
    if (action == "edit.tie_to_entity") return tieToEntity().has_value();
    if (action == "edit.move_to_world") return moveToWorld().has_value();
    if (action == "view.hide") return hideSelection().has_value();
    if (action == "view.unhide") return unhideAll().has_value();
    if (action == "view.hide_unselected") return hideUnselected().has_value();
    if (action == "view.isolate") return toggleIsolate();
    if (action == "edit.undo" || action == "edit.redo") {
        const bool changed = action == "edit.undo" ? document_->undo() : document_->redo();
        if (changed) restoreFromUndo(*document_, selection_);
        return changed;
    }
    if (action == "selection.all") selectAll(selection_, *picks_, *groups_);
    else if (action == "selection.none") selection_.clear();
    else if (action == "selection.invert") invert(selection_, *picks_, *groups_);
    else if (action == "selection.ignore_groups") selection_.setIgnoreGroups(!selection_.ignoreGroups());
    else if (action == "selection.marquee_toggle") {
        setMarqueeMode(marqueeMode_ == MarqueeMode::Enclosed ? MarqueeMode::Touching : MarqueeMode::Enclosed);
    } else {
        const std::pair<const char*, SelectionMode> modes[] = {
            {"selection.mode.groups", SelectionMode::Groups}, {"selection.mode.objects", SelectionMode::Objects},
            {"selection.mode.solids", SelectionMode::Solids}, {"selection.mode.faces", SelectionMode::Faces}};
        bool found = false;
        for (const auto& [id, mode] : modes) {
            if (action == id) { selection_.setMode(mode); found = true; break; }
        }
        if (!found) return false;
    }
    selectTool().cycle_ = {};
    clearHover();
    return true;
}
void EditorSession::setGizmoMode(GizmoMode mode) {
    if (transformActive() || mode == gizmoMode_) return;
    gizmoMode_ = mode;
    notify();
}
void EditorSession::setGizmoSpace(GizmoSpace space) {
    if (transformActive() || space == gizmoSpace_) return;
    gizmoSpace_ = space;
    notify();
}
void EditorSession::setPivotMode(PivotMode mode) {
    if (transformActive() || mode == pivotMode_) return;
    pivotMode_ = mode;
    notify();
}
void EditorSession::setCustomPivot(DVec3 pivot) {
    if (transformActive() || pivot == customPivot_) return;
    customPivot_ = pivot;
    if (pivotMode_ == PivotMode::Custom) notify();
}
void EditorSession::setSnapMode(SnapMode mode) {
    if (mode == snap_.mode) return;
    snap_.mode = mode;
    notify();
}
void EditorSession::setTextureLock(bool lock) {
    if (lock == textureLock_) return;
    textureLock_ = lock;
    notify();
}
void EditorSession::setPivotAtObjectOrigin(bool enabled) {
    if (transformActive() || enabled == pivotAtObjectOrigin_) return;
    pivotAtObjectOrigin_ = enabled;
    notify();
}
void EditorSession::setModelEdgeOverlay(bool enabled) {
    if (enabled == modelEdgeOverlay_) return;
    modelEdgeOverlay_ = enabled;
    notify();
}

bool EditorSession::moveSelection(DVec3 value, bool relative) {
    const auto context = document_ && !transformActive() ? gizmoContext() : std::nullopt;
    if (!context) return false;
    const auto delta = relative ? value : value - context->pivot;
    try { return commitNow(Transform::translate(delta), "Move"); } catch (const std::invalid_argument&) { return false; }
}
bool EditorSession::rotateSelection(DVec3 anglesDeg) {
    const auto context = document_ && !transformActive() ? gizmoContext() : std::nullopt;
    if (!context) return false;
    try {
        const auto pivot = context->pivot;
        const auto transform = Transform::rotate({0, 0, 1}, anglesDeg.z, pivot) *
                               Transform::rotate({0, 1, 0}, anglesDeg.y, pivot) *
                               Transform::rotate({1, 0, 0}, anglesDeg.x, pivot);
        return commitNow(transform, "Rotate");
    } catch (const std::invalid_argument&) { return false; }
}
bool EditorSession::scaleSelection(DVec3 factors) {
    const auto context = document_ && !transformActive() ? gizmoContext() : std::nullopt;
    if (!context) return false;
    try { return commitNow(Transform::scale(factors, context->frame, context->pivot), "Scale"); }
    catch (const std::invalid_argument&) { return false; }
}
bool EditorSession::flipSelection(Axis axis) {
    const auto context = document_ && !transformActive() ? gizmoContext() : std::nullopt;
    if (!context) return false;
    const int index = axis == Axis::X ? 0 : axis == Axis::Y ? 1 : 2;
    try {
        return commitNow(Transform::mirror(context->frame[index], context->pivot),
                         std::string("Flip ") + "XYZ"[index]);
    } catch (const std::invalid_argument&) { return false; }
}
bool EditorSession::nudgeSelection(bool alongV, int direction) {
    if (!document_ || transformActive() || !gizmoContext()) return false;
    try {
        return commitNow(Transform::translate(nudgeDelta(workPlane_, fallbackPlane_, alongV, gridSize_, direction)),
                         "Nudge");
    } catch (const std::invalid_argument&) { return false; }
}

void EditorSession::edit(std::string label, const std::function<void(document::Transaction&)>& operation,
                         std::string coalesceKey) {
    if (!document_) return;
    const Selection before(selection_);
    auto transaction = document_->begin(std::move(label));
    operation(transaction);
    Selection after(selection_);
    after.prune(*document_);
    recordSelection(transaction, before, after);
    const bool coalesce = !coalesceKey.empty();
    transaction.commit(coalesce, std::move(coalesceKey));
}
Result<void> EditorSession::runEdit(std::string label, AfterEdit policy,
                                    const std::function<Result<EditedObjects>(document::Transaction&)>& stage,
                                    std::string coalesceKey) {
    if (!document_) return std::unexpected(Error{ErrorCode::InvalidArgument, "no document"});
    if (transformActive()) return std::unexpected(Error{ErrorCode::InvalidArgument, "a transform is active"});
    const Selection before(selection_);
    try {
        auto transaction = document_->begin(std::move(label));
        auto staged = stage(transaction); // An error returns with the transaction dropped: rolled back.
        if (!staged) return std::unexpected(staged.error());
        Selection after(selection_);
        if (policy == AfterEdit::Computed) after = selectionFor(selection_, *staged);
        else if (policy == AfterEdit::Clear) after.clear();
        after.prune(*document_);
        if (policy == AfterEdit::Visible) {
            auto targets = after.ordered();
            std::erase_if(targets, [&](const Target& target) {
                if (const auto* node = std::get_if<NodeId>(&target)) {
                    if (!isVisible(*document_, *node)) return true;
                    if (std::holds_alternative<Handle<document::Group>>(*document_->handle(*node))) {
                        const auto members = groups_->members(*node, true);
                        return !members.empty() && std::ranges::none_of(members, [&](NodeId member) {
                            return !std::holds_alternative<Handle<document::Group>>(*document_->handle(member)) &&
                                isVisible(*document_, member);
                        });
                    }
                    return false;
                }
                const auto face = document_->side(std::get<SideId>(target));
                return !face || !isVisible(*document_, document_->get(face->solid)->id);
            });
            after.replace(targets);
        }
        recordSelection(transaction, before, after);
        const bool coalesce = !coalesceKey.empty();
        transaction.commit(coalesce, std::move(coalesceKey));
        selection_ = after;
        notify();
    } catch (const std::exception& error) {
        return std::unexpected(Error{ErrorCode::Internal, error.what()});
    }
    return {};
}
void EditorSession::setEntityClasses(assets::EntityClassTable classes) {
    entityClasses_ = std::move(classes);
    ++entityClassesRevision_;
    if (picks_) picks_->rebuild();
    notify();
}
ConnectionModel EditorSession::connectionModel() const {
    if (!document_) throw std::logic_error("no document");
    return ConnectionModel(*document_, selection_);
}
Result<void> EditorSession::setConnectionField(std::size_t row, ConnectionModel::Field field, std::string text) {
    return runEdit("Edit connection", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto result = connectionModel().setField(tx, row, field, std::move(text));
        if (!result) return std::unexpected(result.error());
        return EditedObjects{};
    });
}
Result<void> EditorSession::setConnectionOutput(std::size_t row, std::string name) {
    return runEdit("Rename output", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto result = connectionModel().setOutput(tx, row, std::move(name));
        if (!result) return std::unexpected(result.error());
        return EditedObjects{};
    });
}
Result<void> EditorSession::addConnection(std::string output, document::Connection connection) {
    return runEdit("Add connection", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto result = connectionModel().add(tx, std::move(output), std::move(connection));
        if (!result) return std::unexpected(result.error());
        return EditedObjects{};
    });
}
Result<void> EditorSession::removeConnection(std::size_t row) {
    return runEdit("Delete connection", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto result = connectionModel().remove(tx, row);
        if (!result) return std::unexpected(result.error());
        return EditedObjects{};
    });
}
Result<void> EditorSession::duplicateConnection(std::size_t row) {
    return runEdit("Duplicate connection", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto result = connectionModel().duplicate(tx, row);
        if (!result) return std::unexpected(result.error());
        return EditedObjects{};
    });
}
InspectorModel EditorSession::inspectorModel(bool raw) const {
    if (!document_) throw std::logic_error("no document");
    return InspectorModel(*document_, selection_, entityClasses_, raw);
}
Result<void> EditorSession::editProperty(std::size_t row, std::string text, std::string focusKey, bool raw) {
    return runEdit("Edit property", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto result = inspectorModel(raw).setProperty(tx, row, std::move(text));
        if (!result) return std::unexpected(result.error());
        return EditedObjects{};
    }, inspectorCoalesceKey(row, focusKey, raw));
}
std::string EditorSession::inspectorCoalesceKey(std::size_t row, std::string_view focusKey, bool raw) const {
    if (focusKey.empty()) return {};
    std::string key = "inspector:" + std::string(focusKey) + ":" + std::to_string(row) +
                      (raw ? ":raw:" : ":typed:");
    for (const auto id : selectedNodes(selection_)) key += std::to_string(id) + ",";
    return key;
}
Result<void> EditorSession::editVectorComponent(std::size_t row, std::size_t component,
                                               std::string text, std::string focusKey) {
    return runEdit("Edit property", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto result = inspectorModel().setVectorComponent(tx, row, component, std::move(text));
        if (!result) return std::unexpected(result.error());
        return EditedObjects{};
    }, inspectorCoalesceKey(row, focusKey));
}
Result<void> EditorSession::toggleFlag(std::size_t row, std::uint64_t bit, bool enabled, std::string focusKey) {
    return runEdit("Edit flags", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto result = inspectorModel().toggleFlag(tx, row, bit, enabled);
        if (!result) return std::unexpected(result.error());
        return EditedObjects{};
    }, inspectorCoalesceKey(row, focusKey));
}
Result<void> EditorSession::setChoice(std::size_t row, std::string value, std::string focusKey) {
    return runEdit("Edit choice", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto result = inspectorModel().setChoice(tx, row, std::move(value));
        if (!result) return std::unexpected(result.error());
        return EditedObjects{};
    }, inspectorCoalesceKey(row, focusKey));
}
Result<void> EditorSession::setClass(std::string classname) {
    return runEdit("Change class", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto result = inspectorModel().setClass(tx, std::move(classname));
        if (!result) return std::unexpected(result.error());
        return EditedObjects{};
    });
}
Result<void> EditorSession::placeEntity(std::string classname, DVec3 hit,
                                       std::optional<DVec3> normal, bool align) {
    return runEdit("Place entity", AfterEdit::Computed, [&](document::Transaction& tx) -> Result<EditedObjects> {
        const auto* cls = entityClasses_.find(classname);
        if (!cls || !ciEquals(cls->type, "PointClass"))
            return std::unexpected(Error{ErrorCode::InvalidArgument, "choose a point entity class"});
        AABB bounds{{-4, -4, -4}, {4, 4, 4}};
        if (cls->mins && cls->maxs) {
            bounds.min = {(*cls->mins)[0], (*cls->mins)[1], (*cls->mins)[2]};
            bounds.max = {(*cls->maxs)[0], (*cls->maxs)[1], (*cls->maxs)[2]};
        }
        if (!normal) {
            const auto resolved = workPlane_.resolve();
            const auto& plane = resolved ? resolved->plane : fallbackPlane_;
            hit -= plane.normal * glm::dot(hit - plane.origin, plane.normal);
            for (const auto axis : {plane.uAxis, plane.vAxis}) {
                const double distance = glm::dot(hit - plane.origin, axis);
                hit += axis * (std::round(distance / gridSize_) * gridSize_ - distance);
            }
            normal = plane.normal;
        }
        auto placement = placeOnSurface(hit, *normal, bounds, align);
        if (!placement) return std::unexpected(placement.error());
        const auto text = [](DVec3 v) {
            std::ostringstream stream;
            stream << std::setprecision(17) << v.x << ' ' << v.y << ' ' << v.z;
            return stream.str();
        };
        auto block = kv1::Node::block("entity");
        block.append(kv1::Node::value("classname", cls->name)).value();
        block.append(kv1::Node::value("origin", text(placement->origin))).value();
        if (placement->angles) block.append(kv1::Node::value("angles", text(*placement->angles))).value();
        const auto handle = tx.create(std::move(block));
        EditedObjects result;
        result.items.push_back({document_->get(handle)->id, EditedObjects::Item::Kind::Entity, {}, {}});
        return result;
    });
}
Result<void> EditorSession::placeEntity(std::string classname, Ray ray, bool align) {
    if (!document_) return std::unexpected(Error{ErrorCode::InvalidArgument, "no document"});
    for (const auto& hit : picks_->pick(ray)) {
        if (hit.side || hit.modelTriangle) return placeEntity(std::move(classname), hit.point, hit.normal, align);
    }
    const auto resolved = workPlane_.resolve();
    const auto& plane = resolved ? resolved->plane : fallbackPlane_;
    const double denominator = glm::dot(plane.normal, ray.dir);
    if (std::abs(denominator) < 1e-12)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "ray is parallel to work plane"});
    const double t = glm::dot(plane.origin - ray.origin, plane.normal) / denominator;
    if (t < 0) return std::unexpected(Error{ErrorCode::InvalidArgument, "work plane is behind ray"});
    return placeEntity(std::move(classname), ray.origin + ray.dir * t, std::nullopt, align);
}
Result<std::string> EditorSession::copyText() const {
    if (!document_) return std::unexpected(Error{ErrorCode::InvalidArgument, "no document"});
    return serializeSelection(*document_, *groups_, selection_);
}
Result<std::string> EditorSession::cutText() {
    auto text = copyText();
    if (!text) return text;
    if (auto done = runEdit("Cut", AfterEdit::Clear, [&](document::Transaction& tx) {
            return stageDelete(*document_, *groups_, selection_, tx);
        }); !done)
        return std::unexpected(done.error());
    return text;
}
Result<void> EditorSession::pasteText(std::string_view text, PasteOptions options) {
    if (!document_) return std::unexpected(Error{ErrorCode::InvalidArgument, "no document"});
    if (!options.plane) {
        const auto resolved = workPlane_.resolve();
        options.plane = resolved ? resolved->plane : fallbackPlane_;
    }
    if (!options.snap) options.snap = snap_;
    if (!options.textureLock) options.textureLock = textureLock_;
    options.classes = &entityClasses_;
    return runEdit(options.special ? "Paste special" : "Paste", AfterEdit::Computed, [&](document::Transaction& tx) {
        return stagePaste(*document_, tx, text, options);
    });
}
Result<void> EditorSession::duplicateSelection() {
    auto text = copyText(); // Not the system clipboard: duplicate never touches it.
    if (!text) return std::unexpected(text.error());
    PasteOptions options;
    options.inPlace = true;
    options.textureLock = textureLock_;
    options.classes = &entityClasses_;
    return runEdit("Duplicate", AfterEdit::Computed, [&](document::Transaction& tx) {
        return stagePaste(*document_, tx, *text, options);
    });
}
Result<void> EditorSession::deleteSelection() {
    return runEdit("Delete", AfterEdit::Clear, [&](document::Transaction& tx) {
        return stageDelete(*document_, *groups_, selection_, tx);
    });
}
Result<void> EditorSession::groupSelection() {
    const bool grouped = selection_.mode() == SelectionMode::Groups && !selection_.ignoreGroups();
    return runEdit("Group", grouped ? AfterEdit::Computed : AfterEdit::Keep, [&](document::Transaction& tx) {
        return stageGroup(*document_, *groups_, selection_, tx);
    });
}
Result<void> EditorSession::ungroupSelection() {
    const bool grouped = selection_.mode() == SelectionMode::Groups && !selection_.ignoreGroups();
    return runEdit("Ungroup", grouped ? AfterEdit::Computed : AfterEdit::Keep, [&](document::Transaction& tx) {
        return stageUngroup(*document_, *groups_, selection_, tx);
    });
}
Result<void> EditorSession::tieToEntity(const TieParams& params) {
    return runEdit("Tie to entity", AfterEdit::Computed, [&](document::Transaction& tx) {
        return stageTieToEntity(*document_, *groups_, selection_, params, tx);
    });
}
Result<void> EditorSession::moveToWorld() {
    return runEdit("Move to world", AfterEdit::Computed, [&](document::Transaction& tx) {
        return stageMoveToWorld(*document_, *groups_, selection_, tx);
    });
}
Result<void> EditorSession::moveNodesInto(std::vector<NodeId> nodes, NodeId container) {
    return runEdit("Move in outliner", AfterEdit::Keep, [&](document::Transaction& tx) {
        return stageMoveNodesInto(*document_, *groups_, std::move(nodes), container, tx);
    });
}
RenamePlan EditorSession::planEntityRename(NodeId entity, std::string_view targetname) const {
    if (!document_) return {};
    return planRename(*document_, entityClasses_, entity, targetname);
}
Result<void> EditorSession::renameEntity(NodeId entity, std::string_view targetname) {
    return runEdit("Rename entity", AfterEdit::Keep, [&](document::Transaction& tx) {
        return stageRenameEntity(*document_, entity, targetname, tx);
    });
}
Result<void> EditorSession::renameEntity(NodeId entity, std::string_view targetname, bool updateReferences) {
    if (!updateReferences) return renameEntity(entity, targetname);
    const auto plan = planEntityRename(entity, targetname);
    return runEdit("Rename entity", AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto staged = stageRename(*document_, plan, entity, tx);
        if (!staged) return std::unexpected(staged.error());
        return EditedObjects{};
    });
}
Result<void> EditorSession::setNodeHidden(std::vector<NodeId> nodes, bool hidden) {
    return runEdit(hidden ? "Hide" : "Unhide", AfterEdit::Keep, [&](document::Transaction& tx) {
        return stageSetNodeHidden(*document_, *groups_, nodes, hidden, tx);
    });
}
bool EditorSession::nodeHidden(NodeId node) const {
    return document_ && !isVisible(*document_, node);
}
Result<void> EditorSession::hideSelection() {
    return runEdit("Hide", AfterEdit::Clear, [&](document::Transaction& tx) {
        return stageHide(*document_, *groups_, selection_, tx);
    });
}
Result<void> EditorSession::unhideAll() {
    auto result = runEdit("Unhide all", AfterEdit::Computed, [&](document::Transaction& tx) {
        return stageUnhideAll(*document_, tx);
    });
    if (result) {
        visgroupVisibility_ = {};
        visgroupHistory_[document_->historyState()] = visgroupVisibility_;
        setViewState({});
    }
    return result;
}
Result<void> EditorSession::hideUnselected() {
    return runEdit("Hide unselected", AfterEdit::Keep, [&](document::Transaction& tx) {
        return stageHideUnselected(*document_, *groups_, selection_, tx);
    });
}

Result<void> EditorSession::runVisgroupEdit(std::string label,
    const std::function<Result<void>(document::Transaction&, VisgroupVisibility&)>& stage, bool pruneHidden) {
    const auto before = visgroupVisibility_;
    auto result = runEdit(std::move(label), pruneHidden ? AfterEdit::Visible : AfterEdit::Keep,
        [&](document::Transaction& tx) -> Result<EditedObjects> {
            auto done = stage(tx, visgroupVisibility_);
            if (!done) return std::unexpected(done.error());
            return EditedObjects{};
        });
    if (!result) visgroupVisibility_ = before;
    else visgroupHistory_[document_->historyState()] = visgroupVisibility_;
    return result;
}
Result<int> EditorSession::createVisgroup(std::string name, std::optional<int> parent) {
    int id = 0;
    const auto result = runVisgroupEdit("New visgroup", [&](auto& tx, auto&) -> Result<void> {
        const auto created = stageCreateVisgroup(*document_, std::move(name), parent, tx);
        if (!created) return std::unexpected(created.error());
        id = *created;
        return {};
    });
    if (!result) return std::unexpected(result.error());
    return id;
}
Result<void> EditorSession::renameVisgroup(int id, std::string name) {
    return runVisgroupEdit("Rename visgroup", [&](auto& tx, auto&) {
        return stageRenameVisgroup(*document_, id, std::move(name), tx);
    });
}
Result<void> EditorSession::moveVisgroup(int id, std::optional<int> parent, std::size_t position) {
    return runVisgroupEdit("Move visgroup", [&](auto& tx, auto&) {
        return stageMoveVisgroup(*document_, id, parent, position, tx);
    });
}
Result<void> EditorSession::deleteVisgroup(int id) {
    return runVisgroupEdit("Delete visgroup", [&](auto& tx, auto& state) {
        return stageDeleteVisgroup(*document_, id, state, tx);
    });
}
Result<void> EditorSession::addSelectionToVisgroup(int id) {
    return runVisgroupEdit("Add to visgroup", [&](auto& tx, auto&) {
        return stageVisgroupMembership(*document_, *groups_, selection_, id, true, tx);
    });
}
Result<void> EditorSession::removeSelectionFromVisgroup(int id) {
    return runVisgroupEdit("Remove from visgroup", [&](auto& tx, auto&) {
        return stageVisgroupMembership(*document_, *groups_, selection_, id, false, tx);
    });
}
Result<void> EditorSession::selectVisgroupMembers(int id) {
    if (!document_) return std::unexpected(Error{ErrorCode::InvalidArgument, "no document"});
    const auto tree = buildVisgroupTree(*document_);
    if (!tree.find(id)) return std::unexpected(Error{ErrorCode::NotFound, "unknown visgroup"});
    selection_.setMode(SelectionMode::Objects);
    std::vector<Target> members;
    for (const auto member : tree.members(id)) members.emplace_back(member);
    selection_.replace(members);
    return {};
}
Result<void> EditorSession::selectAutoMembers(std::string_view id) {
    if (!document_) return std::unexpected(Error{ErrorCode::InvalidArgument, "no document"});
    const auto definitions = autoVisgroups();
    if (std::ranges::none_of(definitions, [&](const auto& a) { return a.id == id; }))
        return std::unexpected(Error{ErrorCode::NotFound, "unknown auto visgroup"});
    selection_.setMode(SelectionMode::Objects);
    std::vector<Target> members;
    for (const auto member : autoMembers(*document_, id)) members.emplace_back(member);
    selection_.replace(members);
    return {};
}
Result<void> EditorSession::setVisgroupShown(int id, bool shown) {
    return runVisgroupEdit(shown ? "Show visgroup" : "Hide visgroup", [&](auto& tx, auto& state) {
        return stageSetVisgroupShown(*document_, id, shown, state, tx);
    }, !shown);
}
Result<void> EditorSession::setAutoVisgroupShown(std::string_view id, bool shown) {
    return runVisgroupEdit(shown ? "Show auto visgroup" : "Hide auto visgroup", [&](auto& tx, auto& state) {
        return stageSetAutoVisgroupShown(*document_, id, shown, state, tx);
    }, !shown);
}

bool EditorSession::jumpHistory(std::size_t undoCount) {
    if (!document_) return false;
    if (undoCount == document_->undoCount()) return true;
    if (!document_->jumpTo(undoCount)) return false;
    restoreFromUndo(*document_, selection_);
    return true;
}

bool EditorSession::toggleIsolate() {
    if (!document_ || transformActive()) return false;
    if (view_.isolate) {
        setViewState({});
        return true;
    }
    const auto targets = visibilityTargets(*document_, *groups_, selection_);
    if (targets.empty()) return false;
    ViewState view;
    view.isolate.emplace(targets.begin(), targets.end());
    setViewState(std::move(view));
    return true;
}
std::size_t EditorSession::observe(Observer observer) {
    const auto token = nextObserver_++;
    observers_.emplace(token, std::move(observer));
    return token;
}
void EditorSession::unobserve(std::size_t token) { observers_.erase(token); }
void EditorSession::notify() {
    const auto snapshot = observers_;
    for (const auto& [token, callback] : snapshot) {
        if (observers_.contains(token)) callback();
    }
}

void EditorSession::pointerHover(Ray ray) { activeToolObject().pointerHover(ray); }

void EditorSession::pointerClick(Ray ray, Modifiers modifiers, bool sameSpot) {
    activeToolObject().pointerClick(ray, modifiers, sameSpot);
}

void EditorSession::marquee(std::span<const Plane> planes, Modifiers modifiers) {
    activeToolObject().marquee(planes, modifiers);
}

std::optional<GizmoContext> EditorSession::gizmoContext() const {
    if (activeTool_ != Tool::Select) return {};
    return selectTool().gizmoContext();
}

void EditorSession::endTransform() { activeToolObject().endTransform(); }

void EditorSession::cancelTransform() { activeToolObject().cancelTransform(); }

bool EditorSession::commitNow(const Transform& transform, std::string label) {
    const auto nodes = selectTool().transformNodes();
    if (nodes.empty()) return false;
    if (transform.matrix() == DMat4{1.0}) return true;
    return commitTransform(*document_, nodes, transform, {textureLock_, &entityClasses_}, std::move(label)).has_value();
}

const TransformReadout& EditorSession::activeReadout() const { return selectTool().activeReadout(); }

bool EditorSession::beginGizmoDrag(GizmoHandle handle, const GizmoView& view, Ray ray) {
    return activeTool_ == Tool::Select && selectTool().beginGizmoDrag(handle, view, ray);
}

bool EditorSession::updateGizmoDrag(Ray ray, TransformModifiers modifiers) {
    return selectTool().updateGizmoDrag(ray, modifiers);
}

bool EditorSession::endGizmoDrag() { return selectTool().endGizmoDrag(); }

bool EditorSession::beginModal(GizmoMode mode, const GizmoView& view, Ray ray, DVec2 screenAnchor) {
    return activeTool_ == Tool::Select && selectTool().beginModal(mode, view, ray, screenAnchor);
}

bool EditorSession::updateModal(Ray ray, TransformModifiers modifiers) {
    return selectTool().updateModal(ray, modifiers);
}

ModalEvent EditorSession::modalKey(std::string_view key, bool shift) {
    return selectTool().modalKey(key, shift);
}

editor::Tool& EditorSession::activeToolObject() { return *tools_.at(activeTool_); }
const editor::Tool& EditorSession::activeToolObject() const { return *tools_.at(activeTool_); }
SelectTool& EditorSession::selectTool() { return static_cast<SelectTool&>(*tools_.at(Tool::Select)); }
const SelectTool& EditorSession::selectTool() const { return static_cast<const SelectTool&>(*tools_.at(Tool::Select)); }
bool EditorSession::transformActive() const { return activeToolObject().transformActive(); }
const ModalTransform& EditorSession::modal() const { return selectTool().modal(); }
const CycleState& EditorSession::cycle() const { return selectTool().cycle_; }
void EditorSession::forgetInputSource(InputSource source) {
    if (pieSource_ == source) pieFocusLost();
    for (auto& [id, tool] : tools_) tool->forget(source);
}
void EditorSession::finishPie(PieResult result) {
    if (result.outcome == PieResult::Pending) return;
    if (pieTrace_) pieTrace_(result.outcome == PieResult::Pick ? "pick" : "cancel",
                             result.action.value_or(""), clock_());
    if (pie_->triggerHeld()) pieRelease_ = pie_->trigger();
    pie_.reset();
    pieSource_.reset();
    if (result.outcome == PieResult::Pick && result.action) perform(*result.action);
    notify();
}
void EditorSession::tickPie() {
    if (pie_) finishPie(pie_->tick(clock_()));
}
void EditorSession::pieFocusLost() {
    if (!pie_) return;
    finishPie(pie_->focusLost());
}
bool EditorSession::input(const InputEvent& event, const ToolView& view) {
    // Modifiers can be released before the trigger, so match its physical key from any pane.
    if (event.kind == InputKind::KeyDown && !event.autoRepeat && pieRelease_ &&
        event.chord && event.chord->key == pieRelease_->key) pieRelease_.reset();
    if (event.kind == InputKind::KeyUp && pieRelease_ &&
        (event.chord ? event.chord->key : event.key) == pieRelease_->key) {
        if (!event.autoRepeat) pieRelease_.reset();
        return true;
    }
    if (pie_) {
        finishPie(pie_->input(event, clock_()));
        return true;
    }
    if (event.kind == InputKind::KeyDown && !event.autoRepeat && event.chord) {
        const auto action = keymap_.lookup(*event.chord, BindingContext::forTool(activeTool_));
        if (action == "pie.tools") {
            if (event.navigating) pieRefusal_ = "Stop navigating first.";
            else if (activeToolObject().owns(event.source) || transformActive())
                pieRefusal_ = "Finish the current gesture first.";
            else {
                pieRefusal_.clear();
                pie_ = std::make_unique<PieGesture>(toolPie(), pieSettings_, event.cursor, *event.chord,
                    [this](const ActionId& id) { return isToolEnabled(id); }, clock_());
                pieSource_ = event.source;
                if (pieTrace_) pieTrace_("open", "pie.tools", clock_());
            }
            if (!pieRefusal_.empty() && pieTrace_) pieTrace_("refusal", pieRefusal_, clock_());
            notify();
            return true;
        }
    }
    if (activeToolObject().input(event, view)) return true;
    if (event.kind != InputKind::KeyDown || event.autoRepeat || event.navigating || !event.chord) return false;
    const auto action = keymap_.lookup(*event.chord, BindingContext::forTool(activeTool_));
    if (!action) return false;
    // Modal bindings still consume the key when the active tool has nothing transformable.
    if (document_ && !transformActive() && action->starts_with("transform.modal.")) return true;
    return perform(*action);
}

} // namespace apex::editor
