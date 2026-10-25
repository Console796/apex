#pragma once

// Thin Qt adapter for the active editor tool. Gesture state lives in editor/; this API also
// remains available to the 0.2 synthetic-event tests without a Vulkan surface.

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QPointer>
#include <QWidget>
#include <QSizeF>
#include <QString>
#include <optional>

#include "editor/editor_session.hpp"
#include "render/renderer.hpp"
#include "viewport/camera.hpp"

namespace apex::ui {

// Screen-constant gizmo frame data for a pane of `logicalSize`. Perspective panes derive the
// scale from the pivot depth; orthographic panes from the visible height.
editor::ToolView toolViewFor(const viewport::Camera& camera, QSizeF logicalSize, QPointF cursor);
editor::GizmoView gizmoViewFor(const viewport::Camera& camera, QSizeF logicalSize, DVec3 pivot,
                               double sizePixels = 80.0);

// The held modifier bound to snap.suspend in the active preset (Alt in Hammer, Ctrl in Blender).
std::optional<Qt::KeyboardModifier> snapSuspendModifier(const editor::Preset& preset);
editor::TransformModifiers transformModifiers(const editor::Preset& preset, Qt::KeyboardModifiers held);

// Name understood by editor::ModalTransform::key for a Qt key, or empty.
std::string modalKeyName(const QKeyEvent& event);

class TransformInput {
public:
    TransformInput() = default;
    ~TransformInput();
    TransformInput(const TransformInput&) = delete;
    TransformInput& operator=(const TransformInput&) = delete;
    void setWidget(QWidget* widget);
    static QWidget* widgetFor(editor::InputSource source);
    void setSession(editor::EditorSession* session);

    // Both return true when a transform/action consumes the event before camera navigation.
    // Selection input is dispatched too, retaining the 0.2 false return for ordinary clicks.
    // `size` is the pane's logical size; event positions are in the same units.
    bool mouse(const QMouseEvent& event, const viewport::Camera& camera, QSizeF size);
    // `allowStart` is false while the pane is navigating (a camera button is held).
    bool key(const QKeyEvent& event, const viewport::Camera& camera, QSizeF size, bool allowStart);

    // Leave drops gizmo hover; cancel aborts a transform driven by this pane.
    void leave();
    void cancel();
    void cancelPointer(bool resetClickCycle = false);
    editor::ToolOverlay overlay(const viewport::Camera& camera, QSizeF size) const;
    // True while this pane drives the session's gizmo drag or modal transform.
    bool owns() const;
    std::optional<editor::GizmoHandle> hovered() const;
    std::optional<editor::GizmoHandle> dragged() const;
    // True once after state that affects the pane's picture changed.
    bool takeDirty();

    // Appends the gizmo for the session's mode: nothing without a transformable selection or
    // while a modal transform runs. `dpr` scales pixel widths; the list's viewportSize is set.
    void appendOverlay(render::DrawList& list, const viewport::Camera& camera, QSizeF size, double dpr) const;

private:
    editor::InputSource source() const { return reinterpret_cast<editor::InputSource>(this); }
    editor::EditorSession* session_ = nullptr;
    bool dirty_ = false;
    QPointF cursor_;
};

}  // namespace apex::ui
