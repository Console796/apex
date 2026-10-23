#include "ui/transform_input.hpp"

#include "ui/key_chord.hpp"
#include "viewport/pick_math.hpp"

namespace apex::ui {
namespace {

bool isModifierKey(int key) {
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr;
}
editor::PointerButton pointerButton(Qt::MouseButton button) {
    if (button == Qt::LeftButton) return editor::PointerButton::Left;
    if (button == Qt::RightButton) return editor::PointerButton::Right;
    if (button == Qt::MiddleButton) return editor::PointerButton::Middle;
    return editor::PointerButton::None;
}
void modifiers(editor::InputEvent& event, Qt::KeyboardModifiers held) {
    event.ctrl = held.testFlag(Qt::ControlModifier);
    event.shift = held.testFlag(Qt::ShiftModifier);
    event.alt = held.testFlag(Qt::AltModifier);
}
} // namespace

editor::ToolView toolViewFor(const viewport::Camera& camera, QSizeF size, QPointF cursor) {
    editor::ToolView view;
    view.size = {size.width(), size.height()};
    view.cameraPosition = camera.position;
    view.cameraAngles = camera.angles;
    view.cameraForward = camera.basis().forward;
    view.cameraUp = camera.basis().up;
    view.orthographic = camera.isOrtho();
    view.fovYDegrees = camera.fovYDegrees;
    view.orthoHeight = camera.orthoHeight;
    if (size.width() > 0 && size.height() > 0) {
        view.cursorRay = viewport::pixelRay(camera, size.width() / size.height(), cursor.x(), cursor.y(), size.width(), size.height());
        view.rectFrustum = [camera, size](DVec2 start, DVec2 end) {
            return viewport::rectFrustum(camera, size.width() / size.height(), start.x, start.y, end.x, end.y,
                                         size.width(), size.height());
        };
    }
    return view;
}
editor::GizmoView gizmoViewFor(const viewport::Camera& camera, QSizeF size, DVec3 pivot, double sizePixels) {
    return editor::gizmoViewFor(toolViewFor(camera, size, {}), pivot, sizePixels);
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
    editor::InputEvent event;
    modifiers(event, held);
    return editor::transformModifiers(preset, event);
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

TransformInput::~TransformInput() {
    if (session_) session_->forgetInputSource(source());
}
void TransformInput::setSession(editor::EditorSession* session) {
    if (session_ == session) return;
    cancel();
    leave();
    if (session_) session_->forgetInputSource(source());
    session_ = session;
    leave();
    dirty_ = true;
}
bool TransformInput::owns() const { return session_ && session_->activeToolObject().owns(source()); }
std::optional<editor::GizmoHandle> TransformInput::hovered() const {
    return session_ ? session_->activeToolObject().hovered(source()) : std::nullopt;
}
std::optional<editor::GizmoHandle> TransformInput::dragged() const {
    return session_ ? session_->activeToolObject().dragged(source()) : std::nullopt;
}
bool TransformInput::takeDirty() { return std::exchange(dirty_, false); }
void TransformInput::cancel() {
    if (session_) {
        editor::InputEvent event;
        event.kind = editor::InputKind::Cancel;
        event.source = source();
        event.resetClickCycle = true;
        session_->input(event, {});
    }
    dirty_ = true;
}
void TransformInput::cancelPointer(bool resetClickCycle) {
    if (session_) {
        editor::InputEvent event;
        event.kind = editor::InputKind::CancelPointer;
        event.source = source();
        event.resetClickCycle = resetClickCycle;
        session_->input(event, {});
    }
    dirty_ = true;
}
void TransformInput::leave() {
    if (hovered()) dirty_ = true;
    if (session_) {
        editor::InputEvent event;
        event.kind = editor::InputKind::Leave;
        event.source = source();
        event.cursor = {cursor_.x(), cursor_.y()};
        session_->input(event, {});
    }
}
bool TransformInput::mouse(const QMouseEvent& e, const viewport::Camera& camera, QSizeF size) {
    cursor_ = e.position();
    if (!session_ || !session_->document() || size.width() <= 0 || size.height() <= 0) return false;
    editor::InputEvent event;
    event.kind = e.type() == QEvent::MouseButtonPress ? editor::InputKind::Press
                 : e.type() == QEvent::MouseButtonRelease ? editor::InputKind::Release : editor::InputKind::Move;
    event.source = source();
    event.button = pointerButton(e.button());
    event.left = e.buttons().testFlag(Qt::LeftButton);
    event.right = e.buttons().testFlag(Qt::RightButton);
    event.middle = e.buttons().testFlag(Qt::MiddleButton);
    event.anyButton = e.buttons() != Qt::NoButton;
    event.navigating = event.right || event.middle;
    event.cursor = {cursor_.x(), cursor_.y()};
    modifiers(event, e.modifiers());
    auto view = toolViewFor(camera, size, cursor_);
    const auto before = hovered();
    const bool owned = owns();
    const bool consumed = session_->input(event, view);
    dirty_ |= owned || owns() || before != hovered() || event.left || event.button == editor::PointerButton::Left;
    return consumed;
}
bool TransformInput::key(const QKeyEvent& e, const viewport::Camera& camera, QSizeF size, bool allowStart) {
    if (!session_ || size.width() <= 0 || size.height() <= 0) return false;
    editor::InputEvent event;
    event.kind = e.type() == QEvent::KeyPress ? editor::InputKind::KeyDown : editor::InputKind::KeyUp;
    event.source = source();
    event.cursor = {cursor_.x(), cursor_.y()};
    event.key = modalKeyName(e);
    event.chord = keyChord(e);
    event.modifierKey = isModifierKey(e.key());
    event.autoRepeat = e.isAutoRepeat();
    event.navigating = !allowStart;
    modifiers(event, e.modifiers());
    auto view = toolViewFor(camera, size, cursor_);
    const bool owned = owns();
    const bool consumed = session_->input(event, view);
    dirty_ |= owned || owns();
    return consumed;
}
editor::ToolOverlay TransformInput::overlay(const viewport::Camera& camera, QSizeF size) const {
    if (!session_ || size.width() <= 0 || size.height() <= 0) return {};
    auto view = toolViewFor(camera, size, cursor_);
    return session_->activeToolObject().overlay(view, source());
}
void TransformInput::appendOverlay(render::DrawList& list, const viewport::Camera& camera, QSizeF size, double dpr) const {
    if (!session_ || !session_->document() || size.width() <= 0 || size.height() <= 0) return;
    list.viewportSize = Vec2(static_cast<float>(size.width() * dpr), static_cast<float>(size.height() * dpr));
    const auto primitives = overlay(camera, size);
    for (const auto& p : primitives.segments)
        list.overlaySegments.push_back({p.a, p.b, p.colour, p.width * static_cast<float>(dpr)});
    for (const auto& p : primitives.triangles)
        list.overlayTriangles.push_back({p.a, p.b, p.c, p.colour});
}

} // namespace apex::ui
