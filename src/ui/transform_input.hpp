#pragma once

// Pointer and key routing for the transform gizmo and modal transforms of one viewport pane
// (plan 0.2.5 K2). Kept out of PaneWindow so it can be driven with synthetic Qt events without a
// Vulkan surface. The decisions live in editor::EditorSession; this class only turns Qt events
// into rays, handles and key names, and the gizmo description into overlay primitives.

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <optional>

#include "editor/editor_session.hpp"
#include "render/renderer.hpp"
#include "viewport/camera.hpp"

namespace apex::ui {

// Screen-constant gizmo frame data for a pane of `logicalSize`. Perspective panes derive the
// scale from the pivot depth; orthographic panes from the visible height.
editor::GizmoView gizmoViewFor(const viewport::Camera& camera, QSizeF logicalSize, DVec3 pivot,
                               double sizePixels = 80.0);

// The held modifier bound to snap.suspend in the active preset (Alt in Hammer, Ctrl in Blender).
std::optional<Qt::KeyboardModifier> snapSuspendModifier(const editor::Preset& preset);
editor::TransformModifiers transformModifiers(const editor::Preset& preset, Qt::KeyboardModifiers held);

// Name understood by editor::ModalTransform::key for a Qt key, or empty.
std::string modalKeyName(const QKeyEvent& event);

class TransformInput {
public:
    void setSession(editor::EditorSession* session);

    // Both return true when the event was consumed and must not reach selection or navigation.
    // `size` is the pane's logical size; event positions are in the same units.
    bool mouse(const QMouseEvent& event, const viewport::Camera& camera, QSizeF size);
    // `allowStart` is false while the pane is navigating (a camera button is held).
    bool key(const QKeyEvent& event, const viewport::Camera& camera, QSizeF size, bool allowStart);

    // Pointer left the pane or the pane lost focus: drops hover, and cancels a transform it drives.
    void leave();
    void cancel();
    // True while this pane drives the session's gizmo drag or modal transform.
    bool owns() const;
    std::optional<editor::GizmoHandle> hovered() const { return hovered_; }
    std::optional<editor::GizmoHandle> dragged() const { return dragged_; }
    // True once after state that affects the pane's picture changed.
    bool takeDirty();

    // Appends the gizmo for the session's mode: nothing without a transformable selection or
    // while a modal transform runs. `dpr` scales pixel widths; the list's viewportSize is set.
    void appendOverlay(render::DrawList& list, const viewport::Camera& camera, QSizeF size, double dpr) const;

private:
    Ray rayAt(const viewport::Camera& camera, QSizeF size, QPointF at) const;
    void normalise();
    void refresh(const QKeyEvent& event);
    bool updateFromCursor(Qt::KeyboardModifiers held);

    editor::EditorSession* session_ = nullptr;
    bool owns_ = false, modal_ = false, dirty_ = false;
    QPointF cursor_;
    viewport::Camera camera_;
    QSizeF size_;
    std::optional<editor::GizmoHandle> hovered_, dragged_;
};

}  // namespace apex::ui
