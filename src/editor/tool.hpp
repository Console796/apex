#pragma once

#include "editor/actions.hpp"
#include "editor/gizmo.hpp"
#include "editor/preview.hpp"
#include "editor/selection_ops.hpp"

#include <functional>
#include <map>

namespace apex::editor {

class EditorSession;
using ToolId = std::string;
using InputSource = std::uintptr_t;
using Handled = bool;

enum class InputKind { Press, Release, Move, KeyDown, KeyUp, Leave, Cancel, CancelPointer };
enum class PointerButton { None, Left, Right, Middle };

// Logical pixels, y down. source identifies a pane; held buttons include the changed button on
// press, exclude it on release. CancelPointer drops selection only; Cancel also aborts a transform.
struct InputEvent {
    InputKind kind = InputKind::Move;
    InputSource source = 0;
    PointerButton button = PointerButton::None;
    bool left = false, right = false, middle = false;
    bool anyButton = false; // A held button without a field above (back/forward).
    bool ctrl = false, shift = false, alt = false;
    DVec2 cursor{0.0};
    std::string key; // ModalTransform key name; empty for keys it does not understand.
    std::optional<KeyChord> chord;
    bool modifierKey = false, autoRepeat = false, navigating = false;
    bool resetClickCycle = false; // Navigation/focus changes also invalidate same-spot cycling.
    bool held() const { return left || right || middle || anyButton; }
};

// What a tool sees of a viewport. Editor may not include viewport/ (tools/check_layers.py), so the
// UI copies plain camera data and supplies the existing picking math as a callback.
struct ToolView {
    Ray cursorRay;
    DVec2 size{1.0};
    DVec3 cameraPosition{0.0}, cameraAngles{0.0};
    DVec3 cameraForward{1, 0, 0}, cameraUp{0, 0, 1};
    bool orthographic = false;
    double fovYDegrees = 75.0, orthoHeight = 1024.0;
    std::function<std::array<Plane, 4>(DVec2, DVec2)> rectFrustum;
};
// Logical pixels, y down; perspective rejects points on or behind the camera plane.
std::optional<DVec2> worldToPixel(const ToolView& view, DVec3 point);
GizmoView gizmoViewFor(const ToolView& view, DVec3 pivot, double sizePixels = 80.0);
TransformModifiers transformModifiers(const Preset& preset, const InputEvent& event);

// Pixel sizes and offsets are logical pixels. Existing segments/triangles stay always on top.
struct ToolOverlay {
    struct Segment { DVec3 a, b; Vec4 colour; float width; };
    struct Triangle { DVec3 a, b, c; Vec4 colour; };
    std::vector<Segment> segments;
    std::vector<Triangle> triangles;
    struct Point { DVec3 position; Vec4 colour; float size = 7.0f; };
    struct Label { DVec3 position; std::string text; DVec2 offset{0.0}; Vec4 colour{1.0f}; };
    std::vector<Segment> worldSegments;
    std::vector<Point> worldPoints;
    std::vector<Triangle> worldTriangles;
    std::vector<Label> labels;
    std::optional<std::pair<DVec2, DVec2>> rectangle;
};
struct ToolReadout {
    std::vector<std::string> badge; // Translation keys in the existing ui::modes context.
    std::string detail; // Literal suffix (e.g. the entity classname).
    std::string status;
    bool warn = false;
};

class Tool {
public:
    virtual ~Tool() = default;
    virtual ToolId id() const = 0;
    virtual void activate() {}
    virtual void deactivate() { cancel(); }
    virtual Handled input(const InputEvent& event, const ToolView& view) = 0;
    virtual const Preview& preview() const = 0;
    virtual ToolOverlay overlay(const ToolView& view, InputSource source) const = 0;
    virtual ToolReadout readout() const = 0;
    virtual void cancel() = 0;
    virtual std::vector<ActionId> actions() const { return {}; }

    // 0.2 session compatibility entry points, dispatched to the active tool. New tools take input().
    virtual void pointerHover(Ray) {}
    virtual void pointerClick(Ray, Modifiers, bool) {}
    virtual void marquee(std::span<const Plane>, Modifiers) {}
    virtual bool owns(InputSource) const { return false; }
    // The pane behind source is gone: drop its per-pane state.
    virtual void forget(InputSource) {}
    virtual std::optional<GizmoHandle> hovered(InputSource) const { return {}; }
    virtual std::optional<GizmoHandle> dragged(InputSource) const { return {}; }
};

// Shared 0.2 pointer gesture mechanics. Each tool owns its instance and pane histories.
class SelectionGesture {
public:
    void input(const InputEvent& event, const ToolView& view, Tool& tool, EditorSession& session);
    void cancel(InputSource source, bool resetClickCycle = false);
    void clear() { panes_.clear(); }
    void forget(InputSource source) { panes_.erase(source); }
    std::optional<std::pair<DVec2, DVec2>> rectangle(InputSource source) const;
private:
    struct Pane {
        bool pending = false, allowed = false, dragging = false;
        DVec2 start{0.0}, cursor{0.0}, lastClick{0.0}, lastSize{0.0};
        Modifiers modifiers;
        std::optional<ToolView> gestureCamera, clickCamera;
    };
    std::map<InputSource, Pane> panes_;
};

} // namespace apex::editor
