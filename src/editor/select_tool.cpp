#include "editor/select_tool.hpp"
#include "editor/editor_session.hpp"
#include "editor/selection_expand.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace apex::editor {
namespace {
DVec3 anyPerpendicular(DVec3 n) {
    const DVec3 axis = std::abs(n.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{0, 1, 0};
    return glm::normalize(glm::cross(axis, n));
}

}

void SelectTool::pointerHover(Ray ray) {
    std::optional<Target> target;
    std::optional<NodeId> drawable;
    std::optional<std::pair<NodeId, SideId>> face;
    if (session_.picks_) {
        if (const auto hit = session_.picks_->first(ray); hit && hit->side) face = {{hit->node, *hit->side}};
        for (const auto& hit : session_.picks_->pick(ray)) {
            target = resolveClick(hit, session_.selection_.mode(), *session_.groups_, session_.selection_.ignoreGroups());
            if (target) { drawable = hit.node; break; }
        }
    }
    const bool faceChanged = face != session_.hoverFace_;
    if (target == session_.hover_ && drawable == session_.hoverDrawable_ && !faceChanged) return;
    session_.hover_ = target;
    session_.hoverDrawable_ = drawable;
    if (faceChanged) {
        session_.hoverFace_ = face;
        session_.workPlane_.setHovered(face ? planeFromFace(*session_.document_, face->first, face->second) : std::nullopt);
    }
    session_.notify();
}
void SelectTool::pointerClick(Ray ray, Modifiers modifiers, bool sameSpot) {
    if (session_.picks_) click(session_.selection_, *session_.picks_, *session_.groups_, ray, modifiers, &cycle_, sameSpot);
}
void SelectTool::marquee(std::span<const Plane> planes, Modifiers modifiers) {
    cycle_ = {};
    if (session_.picks_) editor::marquee(session_.selection_, *session_.picks_, *session_.groups_, planes, session_.marqueeMode_, modifiers);
}
std::vector<NodeId> SelectTool::transformNodes() const {
    if (!session_.document_ || session_.selection_.mode() == SelectionMode::Faces) return {};
    return expandSelection(*session_.document_, *session_.groups_, session_.selection_).solidsAndEntities;
}
std::optional<GizmoContext> SelectTool::gizmoContext() const {
    const auto nodes = transformNodes();
    if (nodes.empty()) return std::nullopt;
    GizmoContext context;
    AABB bounds;
    for (const auto node : nodes) {
        if (const auto box = session_.picks_->bounds(node)) bounds.expand(*box);
    }
    if (bounds.empty()) return std::nullopt;
    context.bounds = bounds;
    // Active object: its entity key values give the origin and basis; other nodes use bounds.
    DVec3 activeOrigin = bounds.center(), activeAngles{0.0};
    DMat3 local{1.0};
    bool activeEntity = false;
    if (const auto active = session_.selection_.active(); active && std::holds_alternative<NodeId>(*active)) {
        const auto node = std::get<NodeId>(*active);
        if (const auto box = session_.picks_->bounds(node)) activeOrigin = box->center();
        const auto handle = session_.document_->handle(node);
        if (handle && std::holds_alternative<Handle<document::Entity>>(*handle)) {
            const auto& block = *session_.document_->get(*handle)->block;
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
    const bool singleObject = session_.pivotAtObjectOrigin && nodes.size() == 1 && session_.selection_.active() &&
                              std::holds_alternative<NodeId>(*session_.selection_.active()) &&
                              std::get<NodeId>(*session_.selection_.active()) == nodes.front();
    context.pivot = session_.pivotMode_ == PivotMode::Custom ? session_.customPivot_
                    : session_.pivotMode_ == PivotMode::ActiveOrigin || singleObject ? activeOrigin : bounds.center();
    context.localFrame = local;
    context.frame = session_.gizmoSpace_ == GizmoSpace::Local ? local : DMat3{1.0};
    if (session_.gizmoSpace_ == GizmoSpace::Global && activeEntity)
        context.rotationStartDeg = {activeAngles.z, activeAngles.x, activeAngles.y}; // Roll, pitch, yaw.
    const auto resolved = session_.workPlane_.resolve();
    context.workPlane = resolved ? resolved->plane : session_.fallbackPlane_;
    context.snap = session_.snap_;
    context.snap.gridSize = session_.gridSize_;
    return context;
}
void SelectTool::publishPreview(const Transform& transform) {
    preview_ = fromTransform(activeNodes_, transform);
    session_.notify();
}
void SelectTool::endTransform() {
    drag_.cancel();
    modal_.cancel();
    dragging_ = modalActive_ = false;
    activeNodes_.clear();
    owner_.reset();
    for (auto& [source, pane] : panes_) pane.dragged.reset();
    preview_.clear();
}
void SelectTool::cancelTransform() {
    if (!transformActive()) return;
    endTransform();
    session_.notify();
}
bool SelectTool::commitActive(const Transform& transform, GizmoMode mode) {
    endTransform(); // The preview is cleared in the same call as the commit.
    const bool ok = session_.commitNow(transform, mode == GizmoMode::Translate ? "Move"
                                         : mode == GizmoMode::Rotate ? "Rotate" : "Scale");
    session_.notify();
    return ok;
}
const TransformReadout& SelectTool::activeReadout() const {
    return modalActive_ ? modal_.value() : drag_.value();
}
bool SelectTool::beginGizmoDrag(GizmoHandle handle, const GizmoView& view, Ray ray) {
    if (!session_.document_ || transformActive()) return false;
    const auto context = gizmoContext();
    if (!context || !drag_.begin(session_.gizmoMode_, handle, *context, view, ray)) return false;
    dragging_ = true;
    activeMode_ = session_.gizmoMode_;
    activeNodes_ = transformNodes();
    publishPreview(Transform{});
    return true;
}
bool SelectTool::updateGizmoDrag(Ray ray, TransformModifiers modifiers) {
    if (!dragging_) return false;
    publishPreview(drag_.update(ray, modifiers).transform);
    return true;
}
bool SelectTool::endGizmoDrag() {
    if (!dragging_) return false;
    return commitActive(drag_.value().transform, activeMode_);
}
bool SelectTool::beginModal(GizmoMode mode, const GizmoView& view, Ray ray, DVec2 screenAnchor) {
    if (!session_.document_ || transformActive()) return false;
    const auto context = gizmoContext();
    if (!context || !modal_.start(mode, *context, view, ray, screenAnchor)) return false;
    modalActive_ = true;
    activeMode_ = mode;
    activeNodes_ = transformNodes();
    publishPreview(modal_.value().transform);
    return true;
}
bool SelectTool::updateModal(Ray ray, TransformModifiers modifiers) {
    if (!modalActive_) return false;
    publishPreview(modal_.update(ray, modifiers).transform);
    return true;
}
ModalEvent SelectTool::modalKey(std::string_view key, bool shift) {
    if (!modalActive_) return ModalEvent::Ignored;
    const auto event = modal_.key(key, shift);
    if (event == ModalEvent::Commit) commitActive(modal_.value().transform, activeMode_);
    else if (event == ModalEvent::Cancel) cancelTransform();
    else if (event == ModalEvent::Updated) publishPreview(modal_.value().transform);
    return event;
}

void SelectTool::cancel() {
    cancelTransform();
    selectionGesture_.clear();
    panes_.clear();
}
void SelectTool::forget(InputSource source) {
    if (owns(source)) cancelTransform();
    selectionGesture_.forget(source);
    panes_.erase(source);
}
bool SelectTool::owns(InputSource source) const { return owner_ == source && transformActive(); }
std::optional<GizmoHandle> SelectTool::hovered(InputSource source) const {
    const auto it = panes_.find(source);
    return it == panes_.end() ? std::nullopt : it->second.hovered;
}
std::optional<GizmoHandle> SelectTool::dragged(InputSource source) const {
    const auto it = panes_.find(source);
    return it == panes_.end() ? std::nullopt : it->second.dragged;
}
Handled SelectTool::input(const InputEvent& event, const ToolView& view) {
    if (transformInput(event, view)) {
        if (event.kind == InputKind::Press || event.kind == InputKind::Release || event.kind == InputKind::Move)
            selectionGesture_.cancel(event.source);
        return true;
    }
    selectionGesture_.input(event, view, *this, session_);
    return false;
}
bool SelectTool::transformInput(const InputEvent& e, const ToolView& camera) {
    auto& pane = panes_[e.source];
    if (e.kind == InputKind::Cancel) {
        if (owns(e.source)) cancelTransform();
        return false;
    }
    if (e.kind == InputKind::CancelPointer) return false;
    if (e.kind == InputKind::Leave) {
        pane.hovered.reset();
        if (!owns(e.source)) session_.clearHover();
        return false;
    }
    if (!session_.document() || camera.size.x <= 0 || camera.size.y <= 0) return false;
    const bool key = e.kind == InputKind::KeyDown || e.kind == InputKind::KeyUp;
    const auto update = [&] {
        const auto mods = transformModifiers(session_.preset(), e);
        return modalActive_ ? updateModal(camera.cursorRay, mods) : updateGizmoDrag(camera.cursorRay, mods);
    };
    if (owns(e.source)) {
        if (key) {
            if (e.modifierKey) { update(); return false; }
            if (e.kind == InputKind::KeyUp) return modalActive_;
            if (!modalActive_) {
                if (e.key != "Escape") return false;
                cancelTransform();
                return true;
            }
            if (!e.key.empty()) modalKey(e.key, e.shift);
            return true;
        }
        if (e.kind == InputKind::Move) update();
        else if (e.kind == InputKind::Press) {
            if (modalActive_) {
                update();
                if (e.button == PointerButton::Left) modalKey("LeftClick");
                else if (e.button == PointerButton::Right) modalKey("RightClick");
            } else if (e.button == PointerButton::Right) cancelTransform();
        } else if (e.kind == InputKind::Release && !modalActive_ && e.button == PointerButton::Left) {
            update();
            endGizmoDrag();
        }
        return true;
    }
    if (key) {
        if (e.kind != InputKind::KeyDown || e.autoRepeat || e.navigating || transformActive() || !e.chord) return false;
        const auto action = session_.keymap().lookup(*e.chord, BindingContext::SelectToolActive);
        if (!action || !action->starts_with("transform.modal.")) return false;
        const auto mode = *action == "transform.modal.grab" ? GizmoMode::Translate
                          : *action == "transform.modal.rotate" ? GizmoMode::Rotate : GizmoMode::Scale;
        const auto context = gizmoContext();
        if (!context) return true;
        const auto view = gizmoViewFor(camera, context->pivot);
        if (beginModal(mode, view, camera.cursorRay, e.cursor)) {
            owner_ = e.source;
            pane.dragged.reset();
            pane.hovered.reset();
            session_.clearHover();
            update();
        }
        return true;
    }
    const auto context = gizmoContext();
    if (!context || transformActive() || e.navigating) {
        pane.hovered.reset();
        return false;
    }
    const auto view = gizmoViewFor(camera, context->pivot);
    const auto hit = hitTestGizmo(session_.gizmoMode(), *context, view, camera.cursorRay);
    if (e.kind == InputKind::Press && e.button == PointerButton::Left) {
        if (hit && beginGizmoDrag(hit->handle, view, camera.cursorRay)) {
            owner_ = e.source;
            pane.dragged = hit->handle;
            pane.hovered.reset();
            session_.clearHover();
            return true;
        }
        return false;
    }
    if (e.kind == InputKind::Move && !e.held()) {
        pane.hovered = hit ? std::optional<GizmoHandle>(hit->handle) : std::nullopt;
        if (hit) {
            session_.clearHover();
            return true;
        }
    }
    return false;
}
ToolReadout SelectTool::readout() const {
    const char* selection = "";
    switch (session_.selection().mode()) {
    case SelectionMode::Groups: selection = "Groups"; break;
    case SelectionMode::Objects: selection = "Objects"; break;
    case SelectionMode::Solids: selection = "Solids"; break;
    case SelectionMode::Faces: selection = "Faces"; break;
    }
    const char* gizmo = "";
    switch (session_.gizmoMode()) {
    case GizmoMode::Translate: gizmo = "Move"; break;
    case GizmoMode::Rotate: gizmo = "Rotate"; break;
    case GizmoMode::Scale: gizmo = "Scale"; break;
    }
    return {{"Select", selection, gizmo}, {}, transformActive() ? activeReadout().text : "", false};
}

ToolOverlay SelectTool::overlay(const ToolView& camera, InputSource source) const {
    ToolOverlay list;
    list.rectangle = selectionGesture_.rectangle(source);
    if (!session_.document() || camera.size.x <= 0 || camera.size.y <= 0) return list;
    if (session_.modal().active()) return list;
    auto context = session_.gizmoContext();
    if (!context) return list;
    const auto mode = session_.gizmoMode();
    // A translate drag carries its gizmo along; rotate and scale stay at the pivot.
    if (session_.transformActive() && mode == GizmoMode::Translate) {
        const auto& readout = session_.activeReadout();
        context->pivot += readout.delta;
        context->bounds.min += readout.delta;
        context->bounds.max += readout.delta;
    }
    auto view = gizmoViewFor(camera, context->pivot);
    const DVec3 toCamera = camera.orthographic ? -camera.cameraForward : glm::normalize(camera.cameraPosition - context->pivot);
    const auto prims = editor::gizmoDescription(mode, *context, view);
    const auto active = dragged(source) ? dragged(source) : hovered(source);
    auto line = [&](DVec3 a, DVec3 b, Vec4 colour, float w) {
        list.segments.push_back({a, b, colour, w});
    };
    for (const auto& p : prims) {
        const bool highlight = active && *active == p.handle;
        const float w = highlight ? 3.5f : 2.5f;
        switch (p.shape) {
        case GizmoShape::Segment: {
            const auto colour = gizmoHandleColour(session_.overlayPalette(), p.handle, highlight, 1.0f);
            line(p.points[0], p.points[1], colour, w);
            const DVec3 d = p.points[1] - p.points[0];
            const double length = glm::length(d);
            if (length < 1e-9) break;
            const DVec3 dir = d / length;
            DVec3 side = glm::cross(dir, toCamera);
            if (glm::length(side) < 1e-6) side = anyPerpendicular(dir);
            side = glm::normalize(side);
            if (mode == GizmoMode::Translate) {
                const double head = length * 0.18, half = length * 0.06;
                const DVec3 base = p.points[1] - dir * head;
                list.triangles.push_back({p.points[1] + dir * head * 0.4, base + side * half, base - side * half, colour});
            } else if (mode == GizmoMode::Scale) {
                const double half = length * 0.045;
                const DVec3 centre = p.points[1], other = glm::normalize(glm::cross(side, dir)) * half;
                const DVec3 s = side * half;
                list.triangles.push_back({centre - s - other, centre + s - other, centre + s + other, colour});
                list.triangles.push_back({centre - s - other, centre + s + other, centre - s + other, colour});
            }
            break;
        }
        case GizmoShape::Ring: {
            const DVec3 n = glm::normalize(p.normal);
            const DVec3 u = anyPerpendicular(n), v = glm::cross(n, u);
            const auto colour = gizmoHandleColour(session_.overlayPalette(), p.handle, highlight, p.handle == GizmoHandle::Screen ? 0.8f : 1.0f);
            constexpr int kSteps = 64;
            DVec3 previous = p.centre + u * p.radius;
            for (int i = 1; i <= kSteps; ++i) {
                const double a = 2.0 * std::numbers::pi * i / kSteps;
                const DVec3 next = p.centre + (u * std::cos(a) + v * std::sin(a)) * p.radius;
                line(previous, next, colour, w);
                previous = next;
            }
            break;
        }
        case GizmoShape::Quad: {
            const bool plane = p.handle == GizmoHandle::XY || p.handle == GizmoHandle::YZ || p.handle == GizmoHandle::ZX;
            auto fill = gizmoHandleColour(session_.overlayPalette(), p.handle, highlight, plane ? (highlight ? 0.55f : 0.32f) : (highlight ? 0.9f : 0.55f));
            list.triangles.push_back({p.points[0], p.points[1], p.points[2], fill});
            list.triangles.push_back({p.points[0], p.points[2], p.points[3], fill});
            const auto edge = gizmoHandleColour(session_.overlayPalette(), p.handle, highlight, 0.9f);
            for (int i = 0; i < 4; ++i) line(p.points[static_cast<std::size_t>(i)], p.points[static_cast<std::size_t>((i + 1) % 4)], edge, 1.5f);
            break;
        }
        }
    }
    return list;
}

} // namespace apex::editor
