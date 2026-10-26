#pragma once

#include "core/math.hpp"
#include "editor/gizmo.hpp"

namespace apex::editor {

// Viewport overlay colours. The UI feeds these from the theme's viewport tokens; the defaults are the
// built-in themes' values. Alpha is applied by the drawing code, so every colour here is opaque.
struct OverlayPalette {
    Vec4 vertexHandle{0.25f, 0.85f, 1.0f, 1.0f};    // Handles of the active element mode.
    Vec4 vertexSelected{1.0f, 0.85f, 0.2f, 1.0f};   // Selected element handles.
    Vec4 vertexInactive{0.4f, 0.58f, 0.68f, 1.0f};  // Handles of the other element modes.
    Vec4 vertexEdge{0.35f, 0.55f, 0.75f, 1.0f};     // Wireframe of solids being edited.
    Vec4 vertexInvalid{1.0f, 0.15f, 0.15f, 1.0f};   // Moved or absorbed points.
    Vec4 vertexSplit{1.0f, 0.45f, 0.08f, 1.0f};     // Outlines of faces a move splits.
    Vec4 faceHover{1.0f, 0.85f, 0.2f, 1.0f};        // The face a face-mode press pushes or pulls.
    Vec4 gizmoX{0.92f, 0.25f, 0.25f, 1.0f};
    Vec4 gizmoY{0.35f, 0.82f, 0.3f, 1.0f};
    Vec4 gizmoZ{0.3f, 0.5f, 1.0f, 1.0f};
    Vec4 gizmoScreen{0.85f, 0.85f, 0.88f, 1.0f};    // Screen-space and uniform handles.
    Vec4 gizmoHighlight{1.0f, 0.88f, 0.25f, 1.0f};  // Hovered or dragged handle.
    Vec4 clip{1.0f, 0.65f, 0.15f, 1.0f};            // Clip points, plane and kept-both ghost.
    Vec4 clipDiscard{1.0f, 0.2f, 0.2f, 1.0f};       // The half a clip discards.
    Vec4 block{1.0f, 0.65f, 0.15f, 1.0f};           // Block tool base and snap markers.
    bool operator==(const OverlayPalette&) const = default;
};

// A gizmo handle's colour: planes take the colour of the axis they are normal to.
inline Vec4 gizmoHandleColour(const OverlayPalette& palette, GizmoHandle handle, bool highlight, float alpha = 1.0f) {
    Vec4 colour = palette.gizmoScreen;
    if (highlight) colour = palette.gizmoHighlight;
    else if (handle == GizmoHandle::X || handle == GizmoHandle::YZ) colour = palette.gizmoX;
    else if (handle == GizmoHandle::Y || handle == GizmoHandle::ZX) colour = palette.gizmoY;
    else if (handle == GizmoHandle::Z || handle == GizmoHandle::XY) colour = palette.gizmoZ;
    colour.a = alpha;
    return colour;
}

} // namespace apex::editor
