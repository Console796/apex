#include "editor/editor_session.hpp"

#include "core/strings.hpp"
#include "editor/selection_expand.hpp"
#include "editor/selection_undo.hpp"

namespace apex::editor {

EditorSession::EditorSession() : preset_(hammerPreset()), keymap_(preset_) {
    selection_.observe([this](const Selection&) { notify(); });
}
EditorSession::~EditorSession() { detach(); }

void EditorSession::attach(document::Document& doc) {
    detach();
    document_ = &doc;
    picks_ = std::make_unique<PickIndex>(doc);
    groups_ = std::make_unique<GroupIndex>(doc);
    documentObserver_ = doc.observe([this](const document::ChangeSet& changes) {
        picks_->apply(changes);
        groups_->apply(changes);
        if (transformActive()) endTransform(); // Undo/redo/external edits invalidate a drag.
        cycle_ = {};
        hover_.reset();
        hoverDrawable_.reset();
        hoverFace_.reset();
        workPlane_.setHovered(std::nullopt);
        selection_.prune(*document_);
        notify();
    });
    notify();
}
void EditorSession::detach() {
    if (transformActive()) endTransform();
    if (document_) document_->unobserve(documentObserver_);
    document_ = nullptr;
    picks_.reset();
    groups_.reset();
    cycle_ = {};
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
    cycle_ = {};
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
void EditorSession::pointerHover(Ray ray) {
    std::optional<Target> target;
    std::optional<NodeId> drawable;
    std::optional<std::pair<NodeId, SideId>> face;
    if (picks_) {
        if (const auto hit = picks_->first(ray); hit && hit->side) face = {{hit->node, *hit->side}};
        for (const auto& hit : picks_->pick(ray)) {
            target = resolveClick(hit, selection_.mode(), *groups_, selection_.ignoreGroups());
            if (target) { drawable = hit.node; break; }
        }
    }
    const bool faceChanged = face != hoverFace_;
    if (target == hover_ && drawable == hoverDrawable_ && !faceChanged) return;
    hover_ = target;
    hoverDrawable_ = drawable;
    if (faceChanged) {
        hoverFace_ = face;
        workPlane_.setHovered(face ? planeFromFace(*document_, face->first, face->second) : std::nullopt);
    }
    notify();
}
void EditorSession::pinWorkPlane() {
    if (!hoverFace_) return;
    workPlane_.pinHovered();
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
void EditorSession::setGridSize(double units) {
    units = GridLadder::clamp(units);
    if (units == gridSize_) return;
    gridSize_ = units;
    notify();
}
void EditorSession::pointerClick(Ray ray, Modifiers modifiers, bool sameSpot) {
    if (picks_) click(selection_, *picks_, *groups_, ray, modifiers, &cycle_, sameSpot);
}
void EditorSession::marquee(std::span<const Plane> planes, Modifiers modifiers) {
    cycle_ = {};
    if (picks_) editor::marquee(selection_, *picks_, *groups_, planes, marqueeMode_, modifiers);
}
void EditorSession::selectNode(NodeId node, Modifiers modifiers) {
    if (!document_ || !document_->handle(node)) return;
    cycle_ = {};
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
bool EditorSession::perform(const ActionId& action) {
    if (!document_) return false;
    if (action == "grid.larger" || action == "grid.smaller") {
        setGridSize(action == "grid.larger" ? GridLadder::larger(gridSize_) : GridLadder::smaller(gridSize_));
        return true;
    }
    if (action == "workplane.pin") {
        pinWorkPlane();
        return true;
    }
    if (action == "workplane.axis_x" || action == "workplane.axis_y" || action == "workplane.axis_z") {
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
        const auto axis = action == "workplane.axis_x" ? Axis::X : action == "workplane.axis_y" ? Axis::Y : Axis::Z;
        pinAxisWorkPlane(axis, bounds.empty() ? DVec3{0.0} : bounds.center());
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
    if (action == "transform.texture_lock.toggle") {
        setTextureLock(!textureLock_);
        return true;
    }
    if (action == "edit.flip.x" || action == "edit.flip.y" || action == "edit.flip.z")
        return flipSelection(action == "edit.flip.x" ? Axis::X : action == "edit.flip.y" ? Axis::Y : Axis::Z);
    if (action == "nudge.left" || action == "nudge.right" || action == "nudge.up" || action == "nudge.down")
        return nudgeSelection(action == "nudge.up" || action == "nudge.down",
                              action == "nudge.right" || action == "nudge.up" ? 1 : -1);
    // transform.dialog, transform.modal.* and snap.suspend need pointer/UI state: the UI
    // layer calls beginModal(), the dialog backend or TransformModifiers directly.
    if (action == "selection.none" && workPlane_.pinned()) {
        unpinWorkPlane();
        return true;
    }
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
    cycle_ = {};
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

std::vector<NodeId> EditorSession::transformNodes() const {
    if (!document_ || selection_.mode() == SelectionMode::Faces) return {};
    return expandSelection(*document_, *groups_, selection_).solidsAndEntities;
}
std::optional<GizmoContext> EditorSession::gizmoContext() const {
    const auto nodes = transformNodes();
    if (nodes.empty()) return std::nullopt;
    GizmoContext context;
    AABB bounds;
    for (const auto node : nodes) {
        if (const auto box = picks_->bounds(node)) bounds.expand(*box);
    }
    if (bounds.empty()) return std::nullopt;
    context.bounds = bounds;
    // Active object: its entity key values give the origin and basis; other nodes use bounds.
    DVec3 activeOrigin = bounds.center(), activeAngles{0.0};
    DMat3 local{1.0};
    bool activeEntity = false;
    if (const auto active = selection_.active(); active && std::holds_alternative<NodeId>(*active)) {
        const auto node = std::get<NodeId>(*active);
        if (const auto box = picks_->bounds(node)) activeOrigin = box->center();
        const auto handle = document_->handle(node);
        if (handle && std::holds_alternative<Handle<document::Entity>>(*handle)) {
            const auto& block = *document_->get(*handle)->block;
            const auto vector = [&](std::string_view key) -> std::optional<DVec3> {
                const auto* entry = block.find(key);
                const auto parsed = entry ? entry->asVec3() : Result<std::array<double, 3>>{};
                if (!entry || !parsed) return std::nullopt;
                return DVec3{(*parsed)[0], (*parsed)[1], (*parsed)[2]};
            };
            if (const auto origin = vector("origin")) activeOrigin = *origin;
            if (const auto angles = vector("angles")) {
                const auto basis = basisFromAngles(*angles);
                local = DMat3{basis[0], -basis[1], basis[2]}; // Source's right-axis is -Y.
                activeAngles = *angles;
                activeEntity = true;
            }
        }
    }
    context.pivot = pivotMode_ == PivotMode::Custom ? customPivot_
                    : pivotMode_ == PivotMode::ActiveOrigin ? activeOrigin : bounds.center();
    context.localFrame = local;
    context.frame = gizmoSpace_ == GizmoSpace::Local ? local : DMat3{1.0};
    if (gizmoSpace_ == GizmoSpace::Global && activeEntity)
        context.rotationStartDeg = {activeAngles.z, activeAngles.x, activeAngles.y}; // Roll, pitch, yaw.
    const auto resolved = workPlane_.resolve();
    context.workPlane = resolved ? resolved->plane : fallbackPlane_;
    context.snap = snap_;
    context.snap.gridSize = gridSize_;
    return context;
}

void EditorSession::publishPreview(const Transform& transform) {
    preview_ = fromTransform(activeNodes_, transform);
    notify();
}
void EditorSession::endTransform() {
    drag_.cancel();
    modal_.cancel();
    dragging_ = modalActive_ = false;
    activeNodes_.clear();
    preview_.clear();
}
void EditorSession::cancelTransform() {
    if (!transformActive()) return;
    endTransform();
    notify();
}
bool EditorSession::commitNow(const Transform& transform, std::string label) {
    const auto nodes = transformNodes();
    if (nodes.empty()) return false;
    if (transform.matrix() == DMat4{1.0}) return true;
    return commitTransform(*document_, nodes, transform, {textureLock_}, std::move(label)).has_value();
}
bool EditorSession::commitActive(const Transform& transform, GizmoMode mode) {
    endTransform(); // The preview is cleared in the same call as the commit.
    const bool ok = commitNow(transform, mode == GizmoMode::Translate ? "Move"
                                         : mode == GizmoMode::Rotate ? "Rotate" : "Scale");
    notify();
    return ok;
}
const TransformReadout& EditorSession::activeReadout() const {
    return modalActive_ ? modal_.value() : drag_.value();
}
bool EditorSession::beginGizmoDrag(GizmoHandle handle, const GizmoView& view, Ray ray) {
    if (!document_ || transformActive()) return false;
    const auto context = gizmoContext();
    if (!context || !drag_.begin(gizmoMode_, handle, *context, view, ray)) return false;
    dragging_ = true;
    activeMode_ = gizmoMode_;
    activeNodes_ = transformNodes();
    publishPreview(Transform{});
    return true;
}
bool EditorSession::updateGizmoDrag(Ray ray, TransformModifiers modifiers) {
    if (!dragging_) return false;
    publishPreview(drag_.update(ray, modifiers).transform);
    return true;
}
bool EditorSession::endGizmoDrag() {
    if (!dragging_) return false;
    return commitActive(drag_.value().transform, activeMode_);
}
bool EditorSession::beginModal(GizmoMode mode, const GizmoView& view, Ray ray, DVec2 screenAnchor) {
    if (!document_ || transformActive()) return false;
    const auto context = gizmoContext();
    if (!context || !modal_.start(mode, *context, view, ray, screenAnchor)) return false;
    modalActive_ = true;
    activeMode_ = mode;
    activeNodes_ = transformNodes();
    publishPreview(modal_.value().transform);
    return true;
}
bool EditorSession::updateModal(Ray ray, TransformModifiers modifiers) {
    if (!modalActive_) return false;
    publishPreview(modal_.update(ray, modifiers).transform);
    return true;
}
ModalEvent EditorSession::modalKey(std::string_view key, bool shift) {
    if (!modalActive_) return ModalEvent::Ignored;
    const auto event = modal_.key(key, shift);
    if (event == ModalEvent::Commit) commitActive(modal_.value().transform, activeMode_);
    else if (event == ModalEvent::Cancel) cancelTransform();
    else if (event == ModalEvent::Updated) publishPreview(modal_.value().transform);
    return event;
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

void EditorSession::edit(std::string label, const std::function<void(document::Transaction&)>& operation) {
    if (!document_) return;
    const Selection before(selection_);
    auto transaction = document_->begin(std::move(label));
    operation(transaction);
    Selection after(selection_);
    after.prune(*document_);
    recordSelection(transaction, before, after);
    transaction.commit();
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

} // namespace apex::editor
