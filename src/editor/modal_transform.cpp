#include "editor/modal_transform.hpp"

#include <charconv>
#include <cmath>

namespace apex::editor {

bool ModalTransform::start(GizmoMode mode, const GizmoContext& context, const GizmoView& view,
                           Ray ray, DVec2 screenAnchor) {
    mode_ = mode; original_ = context; context_ = context; view_ = view;
    startRay_ = ray_ = ray; screenAnchor_ = screenAnchor;
    handle_ = mode == GizmoMode::Scale ? GizmoHandle::Uniform : GizmoHandle::Screen;
    active_ = valid_ = true; local_ = false; typed_.clear(); modifiers_ = {}; value_ = {};
    rebuild();
    return true;
}
void ModalTransform::rebuild() {
    context_ = original_;
    // Modal axis constraints start in global space; a repeated axis chooses local.
    context_.frame = local_ ? original_.localFrame : DMat3{1.0};
    if (local_) context_.rotationStartDeg = DVec3{0.0};
    // Plane scaling is a modal constraint rather than a drawn gizmo handle.
    auto dragHandle = handle_;
    if (mode_ == GizmoMode::Scale && (handle_ == GizmoHandle::XY || handle_ == GizmoHandle::YZ || handle_ == GizmoHandle::ZX))
        dragHandle = GizmoHandle::Uniform;
    drag_.begin(mode_, dragHandle, context_, view_, startRay_);
    evaluate();
}
const TransformReadout& ModalTransform::update(Ray ray, TransformModifiers modifiers) {
    if (!active_) return value_;
    ray_ = ray; modifiers_ = modifiers; evaluate();
    return value_;
}
void ModalTransform::evaluate() {
    valid_ = true;
    auto mouse = drag_.update(ray_, modifiers_);
    if (typed_.empty()) {
        // Project uniform mouse scale onto the active plane for Shift+axis.
        if (mode_ == GizmoMode::Scale && (handle_ == GizmoHandle::XY || handle_ == GizmoHandle::YZ || handle_ == GizmoHandle::ZX)) {
            auto result = evaluateTransform(mode_, handle_, context_, context_.pivot, mouse.factors, modifiers_, true);
            if (result) value_ = *result;
        } else value_ = mouse;
        return;
    }
    double number = 0;
    const auto parsed = std::from_chars(typed_.data(), typed_.data()+typed_.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != typed_.data()+typed_.size() || !std::isfinite(number)) {
        valid_ = false;
        return;
    }
    DVec3 raw{number};
    if (mode_ == GizmoMode::Translate) {
        DVec3 direction{1, 0, 0};
        if (handle_ == GizmoHandle::X) direction = context_.frame[0];
        else if (handle_ == GizmoHandle::Y) direction = context_.frame[1];
        else if (handle_ == GizmoHandle::Z) direction = context_.frame[2];
        else if (glm::length(mouse.delta) > 1e-10) direction = glm::normalize(mouse.delta);
        else if (handle_ == GizmoHandle::YZ) direction = context_.frame[1];
        else if (handle_ == GizmoHandle::Screen) direction = context_.workPlane.uAxis;
        raw = direction * number;
    }
    const auto normal = view_.cameraPosition-context_.pivot;
    const auto result = evaluateTransform(mode_,handle_,context_,context_.pivot,raw,modifiers_,true,
        glm::length(normal) > 1e-10 ? glm::normalize(normal) : DVec3{0,0,1});
    valid_ = result.has_value();
    if (result) value_ = *result;
}
ModalEvent ModalTransform::key(std::string_view key, bool shift) {
    if (!active_) return ModalEvent::Ignored;
    if (key == "Escape" || key == "RightClick") { cancel(); return ModalEvent::Cancel; }
    if (key == "Enter" || key == "LeftClick") {
        if (!valid_) return ModalEvent::Updated;
        active_ = false; drag_.cancel(); return ModalEvent::Commit;
    }
    if (key == "X" || key == "Y" || key == "Z") {
        const auto axis = key == "X" ? GizmoHandle::X : key == "Y" ? GizmoHandle::Y : GizmoHandle::Z;
        const auto plane = key == "X" ? GizmoHandle::YZ : key == "Y" ? GizmoHandle::ZX : GizmoHandle::XY;
        const auto next = shift ? plane : axis;
        local_ = next == handle_ ? !local_ : false;
        handle_ = next;
        rebuild();
        return ModalEvent::Updated;
    }
    if (key == "Backspace") {
        if (!typed_.empty()) typed_.pop_back();
    } else if (key.size() == 1 && ((key[0] >= '0' && key[0] <= '9') || key == "." || key == "-")) {
        if (key == "-") {
            if (!typed_.empty() && typed_[0] == '-') typed_.erase(0,1);
            else typed_.insert(typed_.begin(), '-');
        } else if (key != "." || typed_.find('.') == std::string::npos) typed_ += key;
    } else return ModalEvent::Ignored;
    evaluate();
    return ModalEvent::Updated;
}
} // namespace apex::editor
