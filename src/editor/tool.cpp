#include "editor/tool.hpp"
#include "editor/editor_session.hpp"

#include <algorithm>
#include <cmath>

namespace apex::editor {
namespace {
bool sameCamera(const ToolView& a, const ToolView& b) {
    return a.cameraPosition == b.cameraPosition && a.cameraAngles == b.cameraAngles &&
           a.orthographic == b.orthographic && a.fovYDegrees == b.fovYDegrees && a.orthoHeight == b.orthoHeight;
}
ToolView cameraSnapshot(ToolView view) {
    view.rectFrustum = {};
    return view;
}
}

std::optional<DVec2> worldToPixel(const ToolView& view, DVec3 point) {
    if (!(view.size.x > 0.0 && view.size.y > 0.0)) return {};
    const auto forward = glm::normalize(view.cameraForward);
    const auto right = glm::normalize(glm::cross(forward, view.cameraUp));
    const auto up = glm::cross(right, forward);
    const auto delta = point - view.cameraPosition;
    const double depth = glm::dot(delta, forward);
    if (!view.orthographic && !(depth > 1e-6)) return {};
    const double height = view.orthographic ? view.orthoHeight :
        2.0 * depth * std::tan(glm::radians(view.fovYDegrees) * 0.5);
    if (!(height > 0.0 && std::isfinite(height))) return {};
    const auto pixel = view.size * 0.5 + DVec2{glm::dot(delta, right), -glm::dot(delta, up)} *
        (view.size.y / height);
    if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y)) return {};
    return pixel;
}

GizmoView gizmoViewFor(const ToolView& camera, DVec3 pivot, double sizePixels) {
    GizmoView view;
    view.sizePixels = sizePixels;
    view.cameraUp = camera.cameraUp;
    const double height = std::max(1.0, camera.size.y);
    if (camera.orthographic) {
        view.pixelsPerWorldUnit = height / std::max(1e-6, camera.orthoHeight);
        view.cameraPosition = pivot - camera.cameraForward * 1024.0;
    } else {
        const double depth = std::max(1e-3, glm::dot(pivot - camera.cameraPosition, camera.cameraForward));
        view.pixelsPerWorldUnit = (height * 0.5) / (std::tan(glm::radians(camera.fovYDegrees) * 0.5) * depth);
        view.cameraPosition = camera.cameraPosition;
    }
    return view;
}

bool actionModifier(const Preset& preset, const InputEvent& event, ActionId action) {
    const auto it = preset.bindings.find(action);
    if (it != preset.bindings.end()) {
        if (const auto* chords = std::get_if<std::vector<KeyChord>>(&it->second.value)) {
            for (const auto& chord : *chords) {
                if (chord.key == "Ctrl") return event.ctrl;
                if (chord.key == "Alt") return event.alt;
                if (chord.key == "Shift") return event.shift;
            }
        }
    }
    return false;
}
TransformModifiers transformModifiers(const Preset& preset, const InputEvent& event) {
    TransformModifiers result;
    result.suspend = actionModifier(preset, event, "snap.suspend");
    result.fine = event.shift;
    return result;
}

void SelectionGesture::cancel(InputSource source, bool resetClickCycle) {
    const auto it = panes_.find(source);
    if (it == panes_.end()) return;
    it->second.pending = it->second.dragging = false;
    it->second.gestureCamera.reset();
    if (resetClickCycle) it->second.clickCamera.reset();
}

void SelectionGesture::input(const InputEvent& e, const ToolView& view, Tool& tool, EditorSession& session) {
    if (e.kind == InputKind::Cancel || e.kind == InputKind::CancelPointer) {
        cancel(e.source, e.resetClickCycle);
        return;
    }
    if (e.kind != InputKind::Press && e.kind != InputKind::Release && e.kind != InputKind::Move) return;
    if (e.navigating) {
        cancel(e.source, true);
        session.clearHover();
        return;
    }
    if (!session.document() || view.size.x <= 0 || view.size.y <= 0) return;
    if (e.kind == InputKind::Release && e.button != PointerButton::Left) return;
    auto& pane = panes_[e.source];
    pane.cursor = e.cursor;
    if (e.kind == InputKind::Press && e.button == PointerButton::Left) {
        pane.pending = true;
        pane.start = e.cursor;
        pane.modifiers = {e.ctrl, e.shift};
        pane.gestureCamera = cameraSnapshot(view);
        tool.pointerHover(view.cursorRay);
        pane.allowed = !session.hoverTarget() || e.ctrl || e.shift;
    } else if (e.kind == InputKind::Move && pane.pending) {
        if (!sameCamera(*pane.gestureCamera, view)) { cancel(e.source); return; }
        const auto delta = e.cursor - pane.start;
        if (pane.allowed && glm::dot(delta, delta) > 9) {
            pane.dragging = true;
            session.clearHover();
        }
    } else if (e.kind == InputKind::Release && pane.pending) {
        if (sameCamera(*pane.gestureCamera, view)) {
            if (pane.dragging) {
                if (view.rectFrustum) tool.marquee(view.rectFrustum(pane.start, e.cursor), pane.modifiers);
                pane.clickCamera.reset();
            } else {
                const auto delta = e.cursor - pane.lastClick;
                const bool sameSpot = pane.clickCamera && sameCamera(*pane.clickCamera, view) &&
                                      pane.lastSize == view.size && glm::dot(delta, delta) <= 9;
                tool.pointerClick(view.cursorRay, pane.modifiers, sameSpot);
                pane.lastClick = e.cursor;
                pane.lastSize = view.size;
                pane.clickCamera = cameraSnapshot(view);
            }
        }
        cancel(e.source);
    } else if (e.kind == InputKind::Move && !e.held()) tool.pointerHover(view.cursorRay);
}

std::optional<std::pair<DVec2, DVec2>> SelectionGesture::rectangle(InputSource source) const {
    const auto it = panes_.find(source);
    if (it == panes_.end() || !it->second.dragging) return {};
    return {{it->second.start, it->second.cursor}};
}

} // namespace apex::editor
