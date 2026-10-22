#pragma once

#include "editor/gizmo.hpp"

namespace apex::editor {

enum class ModalEvent { Ignored, Updated, Commit, Cancel };
// Input names are platform independent: X/Y/Z, digits, '.', '-', Backspace,
// Enter, LeftClick, Escape, RightClick. The adapter handles key repeat policy.
class ModalTransform {
public:
    bool start(GizmoMode mode, const GizmoContext& context, const GizmoView& view,
               Ray ray, DVec2 screenAnchor = DVec2{0.0});
    const TransformReadout& update(Ray ray, TransformModifiers modifiers = {});
    ModalEvent key(std::string_view key, bool shift = false);
    bool active() const { return active_; }
    bool valid() const { return valid_; }
    bool localConstraint() const { return local_; }
    GizmoHandle constraint() const { return handle_; }
    const std::string& typedValue() const { return typed_; }
    const TransformReadout& value() const { return value_; }
    DVec2 screenAnchor() const { return screenAnchor_; }
    void cancel() { active_ = false; drag_.cancel(); }
private:
    GizmoMode mode_ = GizmoMode::Translate;
    GizmoContext original_, context_;
    GizmoView view_;
    Ray startRay_, ray_;
    DVec2 screenAnchor_{0.0};
    GizmoHandle handle_ = GizmoHandle::Screen;
    bool active_ = false, valid_ = true, local_ = false;
    std::string typed_;
    GizmoDrag drag_;
    TransformModifiers modifiers_;
    TransformReadout value_;
    void rebuild();
    void evaluate();
};

} // namespace apex::editor
