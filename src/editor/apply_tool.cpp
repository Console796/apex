#include "editor/apply_tool.hpp"
#include "editor/editor_session.hpp"

namespace apex::editor {
bool rightClickApplies(const Preset& preset) {
    InputEvent anyModifier;
    anyModifier.ctrl = anyModifier.shift = anyModifier.alt = true;
    return !actionModifier(preset, anyModifier, "apply.click");
}
void ApplyTool::activate() { session_.selection().setMode(SelectionMode::Faces); }
Handled ApplyTool::input(const InputEvent& e, const ToolView& view) {
    if (!session_.document() || e.navigating || e.kind != InputKind::Press) return false;
    const bool liftAligned = e.button == PointerButton::Left && e.alt &&
        actionModifier(session_.preset(), e, "apply.lift_aligned") &&
        (rightClickApplies(session_.preset()) || actionModifier(session_.preset(), e, "apply.click"));
    if (liftAligned) {
        notice_ = session_.liftMaterialAt(view.cursorRay, true) ? "Lifted material and alignment source" : "No face under cursor";
        session_.publishToolStatus();
        return true;
    }
    const bool apply = rightClickApplies(session_.preset()) ? e.button == PointerButton::Right :
        e.button == PointerButton::Left && actionModifier(session_.preset(), e, "apply.click");
    if (apply) {
        if (hotspot_) {
            const auto& material = session_.newSolidStyle().material;
            const auto result = session_.applyHotspotAt(view.cursorRay, material, e.alt);
            if (!result) notice_ = result.error().what();
            else if (!*result) notice_ = "No face under cursor";
            else notice_ = e.alt ? "Applied hotspot (alternate group)" : "Applied hotspot";
        } else if (e.alt) {
            const auto result = session_.applyMaterialWrapped(view.cursorRay);
            if (!result) notice_ = result.error().what();
            else if (!*result) notice_ = "No face under cursor";
            else notice_ = result->fellBackToWrap ? "Projected was degenerate; applied with Wrap" : "Applied material with alignment";
        } else {
            notice_ = session_.applyMaterialAt(view.cursorRay, session_.newSolidStyle().material)
                ? "Applied material" : "No face under cursor";
        }
        session_.publishToolStatus();
        return true;
    }
    if (e.button != PointerButton::Left) return false;
    if (e.alt) {
        notice_ = session_.liftMaterialAt(view.cursorRay) ? "Lifted material" : "No face under cursor";
        session_.publishToolStatus();
        return true;
    }
    const auto hit = session_.picks().first(view.cursorRay);
    auto& selection = session_.selection();
    selection.setMode(SelectionMode::Faces);
    if (hit && hit->side) {
        if (e.ctrl) selection.toggle(*hit->side);
        else if (e.shift) selection.add(*hit->side);
        else selection.select(*hit->side);
    } else if (!e.ctrl && !e.shift) selection.clear();
    notice_.clear();
    session_.publishToolStatus();
    return true;
}
bool ApplyTool::setParameter(std::string_view id, double value) {
    if (id != "mode") return false;
    hotspot_ = value >= 0.5;
    notice_.clear();
    session_.publishToolStatus();
    return true;
}
ToolReadout ApplyTool::readout() const {
    const auto material = session_.newSolidStyle().material;
    const auto mode = session_.faceExtrapolation() == FaceExtrapolation::Wrap ? "Wrap"
                    : session_.faceExtrapolation() == FaceExtrapolation::Projected ? "Projected" : "Match";
    const bool right = rightClickApplies(session_.preset());
    const auto bindings = hotspot_
        ? (right ? "RMB apply and fit · Alt+RMB fit to the alternate group · Alt+LMB lift"
                 : "Ctrl+click apply and fit · Ctrl+Alt+click fit to the alternate group · Alt+LMB lift")
        : right ? "RMB apply · Alt+RMB apply aligned · Alt+LMB lift · Shift+Alt+LMB lift aligned"
                : "Ctrl+click apply · Ctrl+Alt+click apply aligned · Alt+LMB lift · Ctrl+Shift+Alt+click lift aligned";
    const auto setting = hotspot_ ? std::string("Hotspot") : std::string("Extrapolation: ") + mode;
    ToolReadout readout{{"Face edit"}, material, notice_.empty() ? setting + " · " + bindings
                                                                 : notice_ + " · " + setting + " · " + bindings};
    readout.parameters.push_back({"mode", "Apply", ToolParameter::Kind::Choice, hotspot_ ? 1.0 : 0.0, 0.0, 1.0,
                                  {"Material", "Hotspot"}});
    return readout;
}
} // namespace apex::editor
