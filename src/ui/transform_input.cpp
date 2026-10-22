#include "ui/transform_input.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "ui/key_chord.hpp"
#include "viewport/pick_math.hpp"

namespace apex::ui {
namespace {

using editor::GizmoHandle;
using editor::GizmoMode;
using editor::GizmoShape;

bool isModifierKey(int key) {
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta ||
           key == Qt::Key_AltGr;
}

Vec4 handleColour(GizmoHandle handle, bool highlight, float alpha) {
    if (highlight) return Vec4(1.0f, 0.88f, 0.25f, alpha);
    switch (handle) {
    case GizmoHandle::X: case GizmoHandle::YZ: return Vec4(0.92f, 0.25f, 0.25f, alpha);
    case GizmoHandle::Y: case GizmoHandle::ZX: return Vec4(0.35f, 0.82f, 0.3f, alpha);
    case GizmoHandle::Z: case GizmoHandle::XY: return Vec4(0.3f, 0.5f, 1.0f, alpha);
    default: return Vec4(0.85f, 0.85f, 0.88f, alpha);
    }
}

DVec3 anyPerpendicular(DVec3 n) {
    const DVec3 axis = std::abs(n.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{0, 1, 0};
    return glm::normalize(glm::cross(axis, n));
}

}  // namespace

editor::GizmoView gizmoViewFor(const viewport::Camera& camera, QSizeF size, DVec3 pivot, double sizePixels) {
    editor::GizmoView view;
    view.sizePixels = sizePixels;
    const auto basis = camera.basis();
    view.cameraUp = basis.up;
    const double height = std::max(1.0, size.height());
    if (camera.isOrtho()) {
        view.pixelsPerWorldUnit = height / std::max(1e-6, camera.orthoHeight);
        view.cameraPosition = pivot - basis.forward * 1024.0;  // Direction only matters.
    } else {
        const double depth = std::max(1e-3, glm::dot(pivot - camera.position, basis.forward));
        view.pixelsPerWorldUnit = (height * 0.5) / (std::tan(glm::radians(camera.fovYDegrees) * 0.5) * depth);
        view.cameraPosition = camera.position;
    }
    return view;
}

std::optional<Qt::KeyboardModifier> snapSuspendModifier(const editor::Preset& preset) {
    const auto it = preset.bindings.find("snap.suspend");
    if (it == preset.bindings.end()) return std::nullopt;
    const auto* chords = std::get_if<std::vector<editor::KeyChord>>(&it->second.value);
    if (!chords) return std::nullopt;
    for (const auto& chord : *chords) {
        if (chord.key == "Ctrl") return Qt::ControlModifier;
        if (chord.key == "Alt") return Qt::AltModifier;
        if (chord.key == "Shift") return Qt::ShiftModifier;
    }
    return std::nullopt;
}

editor::TransformModifiers transformModifiers(const editor::Preset& preset, Qt::KeyboardModifiers held) {
    editor::TransformModifiers result;
    if (const auto suspend = snapSuspendModifier(preset)) result.suspend = held.testFlag(*suspend);
    result.fine = held.testFlag(Qt::ShiftModifier);
    return result;
}

std::string modalKeyName(const QKeyEvent& event) {
    const int key = event.key();
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return std::string(1, static_cast<char>('0' + (key - Qt::Key_0)));
    switch (key) {
    case Qt::Key_X: return "X";
    case Qt::Key_Y: return "Y";
    case Qt::Key_Z: return "Z";
    case Qt::Key_Period: return ".";
    case Qt::Key_Minus: return "-";
    case Qt::Key_Backspace: return "Backspace";
    case Qt::Key_Return: case Qt::Key_Enter: return "Enter";
    case Qt::Key_Escape: return "Escape";
    default: return {};
    }
}

void TransformInput::setSession(editor::EditorSession* session) {
    if (session_ == session) return;
    cancel();
    session_ = session;
    hovered_.reset();
    dirty_ = true;
}

bool TransformInput::owns() const { return owns_ && session_ && session_->transformActive(); }

bool TransformInput::takeDirty() { return std::exchange(dirty_, false); }

void TransformInput::normalise() {
    if (owns_ && !(session_ && session_->transformActive())) {
        owns_ = modal_ = false;
        dragged_.reset();
        dirty_ = true;
    }
}

void TransformInput::cancel() {
    if (owns_ && session_) session_->cancelTransform();
    owns_ = modal_ = false;
    dragged_.reset();
    dirty_ = true;
}

void TransformInput::leave() {
    if (hovered_) dirty_ = true;
    hovered_.reset();
}

Ray TransformInput::rayAt(const viewport::Camera& camera, QSizeF size, QPointF at) const {
    return viewport::pixelRay(camera, size.width() / size.height(), at.x(), at.y(), size.width(), size.height());
}

bool TransformInput::updateFromCursor(Qt::KeyboardModifiers held) {
    if (!owns()) return false;
    const auto ray = rayAt(camera_, size_, cursor_);
    const auto mods = transformModifiers(session_->preset(), held);
    const bool ok = modal_ ? session_->updateModal(ray, mods) : session_->updateGizmoDrag(ray, mods);
    dirty_ = true;
    return ok;
}

bool TransformInput::mouse(const QMouseEvent& e, const viewport::Camera& camera, QSizeF size) {
    cursor_ = e.position();
    camera_ = camera;
    size_ = size;
    if (!session_ || !session_->document() || size.width() <= 0 || size.height() <= 0) return false;
    normalise();
    const auto type = e.type();

    if (owns_) {
        if (type == QEvent::MouseMove) {
            updateFromCursor(e.modifiers());
        } else if (type == QEvent::MouseButtonPress) {
            if (modal_) {
                // Keep the final pointer position before committing.
                updateFromCursor(e.modifiers());
                if (e.button() == Qt::LeftButton) session_->modalKey("LeftClick");
                else if (e.button() == Qt::RightButton) session_->modalKey("RightClick");
            } else if (e.button() == Qt::RightButton) {
                session_->cancelTransform();
            }
        } else if (type == QEvent::MouseButtonRelease && !modal_ && e.button() == Qt::LeftButton) {
            updateFromCursor(e.modifiers());
            session_->endGizmoDrag();
        }
        normalise();
        dirty_ = true;
        return true;
    }

    const auto context = session_->gizmoContext();
    if (!context || session_->transformActive()) {
        if (hovered_) dirty_ = true;
        hovered_.reset();
        return false;
    }
    if (e.buttons().testFlag(Qt::RightButton) || e.buttons().testFlag(Qt::MiddleButton)) {
        if (hovered_) dirty_ = true;
        hovered_.reset();
        return false;
    }
    const auto view = gizmoViewFor(camera, size, context->pivot);
    const auto ray = rayAt(camera, size, cursor_);
    const auto hit = editor::hitTestGizmo(session_->gizmoMode(), *context, view, ray);
    if (type == QEvent::MouseButtonPress && e.button() == Qt::LeftButton) {
        if (hit && session_->beginGizmoDrag(hit->handle, view, ray)) {
            owns_ = true;
            modal_ = false;
            dragged_ = hit->handle;
            hovered_.reset();
            session_->clearHover();
            dirty_ = true;
            return true;
        }
        return false;
    }
    if (type == QEvent::MouseMove && e.buttons() == Qt::NoButton) {
        const std::optional<GizmoHandle> next = hit ? std::optional<GizmoHandle>(hit->handle) : std::nullopt;
        if (next != hovered_) dirty_ = true;
        hovered_ = next;
        if (hit) {
            session_->clearHover();
            return true;  // The gizmo shadows the selection hover underneath it.
        }
    }
    return false;
}

void TransformInput::refresh(const QKeyEvent& event) {
    if (owns()) updateFromCursor(event.modifiers());
}

bool TransformInput::key(const QKeyEvent& e, const viewport::Camera& camera, QSizeF size, bool allowStart) {
    if (!session_ || !session_->document() || size.width() <= 0 || size.height() <= 0) return false;
    camera_ = camera;
    size_ = size;
    normalise();
    const bool press = e.type() == QEvent::KeyPress;
    if (owns_) {
        if (isModifierKey(e.key())) {
            refresh(e);
            return false;
        }
        if (!press) return modal_;
        if (!modal_) {
            if (e.key() != Qt::Key_Escape) return false;
            session_->cancelTransform();
            normalise();
            return true;
        }
        const auto name = modalKeyName(e);
        if (!name.empty()) session_->modalKey(name, e.modifiers().testFlag(Qt::ShiftModifier));
        normalise();
        dirty_ = true;
        return true;
    }
    if (!press || e.isAutoRepeat() || !allowStart || session_->transformActive()) return false;
    const auto chord = keyChord(e);
    if (!chord) return false;
    const auto action = session_->keymap().lookup(*chord, editor::BindingContext::SelectToolActive);
    if (!action || !action->starts_with("transform.modal.")) return false;
    const auto mode = *action == "transform.modal.grab" ? GizmoMode::Translate
                      : *action == "transform.modal.rotate" ? GizmoMode::Rotate : GizmoMode::Scale;
    const auto context = session_->gizmoContext();
    if (!context) return true;  // Nothing to transform; the key is still ours.
    const auto view = gizmoViewFor(camera, size, context->pivot);
    if (session_->beginModal(mode, view, rayAt(camera, size, cursor_), DVec2{cursor_.x(), cursor_.y()})) {
        owns_ = modal_ = true;
        dragged_.reset();
        hovered_.reset();
        session_->clearHover();
        updateFromCursor(e.modifiers());
    }
    return true;
}

void TransformInput::appendOverlay(render::DrawList& list, const viewport::Camera& camera, QSizeF size,
                                   double dpr) const {
    if (!session_ || !session_->document() || size.width() <= 0 || size.height() <= 0) return;
    list.viewportSize = Vec2(static_cast<float>(size.width() * dpr), static_cast<float>(size.height() * dpr));
    if (session_->modal().active()) return;
    auto context = session_->gizmoContext();
    if (!context) return;
    const auto mode = session_->gizmoMode();
    // A translate drag carries its gizmo along; rotate and scale stay at the pivot.
    if (session_->transformActive() && mode == GizmoMode::Translate) {
        const auto& readout = session_->activeReadout();
        context->pivot += readout.delta;
        context->bounds.min += readout.delta;
        context->bounds.max += readout.delta;
    }
    auto view = gizmoViewFor(camera, size, context->pivot);
    const auto basis = camera.basis();
    const DVec3 toCamera = camera.isOrtho() ? -basis.forward : glm::normalize(camera.position - context->pivot);
    const float width = static_cast<float>(dpr);
    const auto prims = editor::gizmoDescription(mode, *context, view);
    const auto active = dragged_ ? dragged_ : hovered_;
    auto line = [&](DVec3 a, DVec3 b, Vec4 colour, float w) {
        list.overlaySegments.push_back({a, b, colour, w * width});
    };
    for (const auto& p : prims) {
        const bool highlight = active && *active == p.handle;
        const float w = highlight ? 3.5f : 2.5f;
        switch (p.shape) {
        case GizmoShape::Segment: {
            const auto colour = handleColour(p.handle, highlight, 1.0f);
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
                list.overlayTriangles.push_back({p.points[1] + dir * head * 0.4, base + side * half, base - side * half, colour});
            } else if (mode == GizmoMode::Scale) {
                const double half = length * 0.045;
                const DVec3 centre = p.points[1], other = glm::normalize(glm::cross(side, dir)) * half;
                const DVec3 s = side * half;
                list.overlayTriangles.push_back({centre - s - other, centre + s - other, centre + s + other, colour});
                list.overlayTriangles.push_back({centre - s - other, centre + s + other, centre - s + other, colour});
            }
            break;
        }
        case GizmoShape::Ring: {
            const DVec3 n = glm::normalize(p.normal);
            const DVec3 u = anyPerpendicular(n), v = glm::cross(n, u);
            const auto colour = handleColour(p.handle, highlight, p.handle == GizmoHandle::Screen ? 0.8f : 1.0f);
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
            auto fill = handleColour(p.handle, highlight, plane ? (highlight ? 0.55f : 0.32f) : (highlight ? 0.9f : 0.55f));
            list.overlayTriangles.push_back({p.points[0], p.points[1], p.points[2], fill});
            list.overlayTriangles.push_back({p.points[0], p.points[2], p.points[3], fill});
            const auto edge = handleColour(p.handle, highlight, 0.9f);
            for (int i = 0; i < 4; ++i) line(p.points[static_cast<std::size_t>(i)], p.points[static_cast<std::size_t>((i + 1) % 4)], edge, 1.5f);
            break;
        }
        }
    }
}

}  // namespace apex::ui
