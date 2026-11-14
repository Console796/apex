#include "editor/displacement_tool.hpp"

#include "editor/displacement_edit.hpp"
#include "editor/editor_session.hpp"
#include "geometry/displacement.hpp"

#include <algorithm>
#include <cmath>

namespace apex::editor {

void DisplacementTool::activate() { session_.selection().setMode(SelectionMode::Faces); }

void DisplacementTool::setMode(Mode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    notice_.clear();
    session_.publishToolStatus();
}

void DisplacementTool::setNotice(std::string notice) {
    notice_ = std::move(notice);
    session_.publishToolStatus();
}

bool DisplacementTool::setParameter(std::string_view id, double value) {
    if (!std::isfinite(value)) return false;
    if (id == "mode") {
        const auto index = std::clamp(static_cast<int>(std::lround(value)), 0, 2);
        setMode(static_cast<Mode>(index));
        return true;
    }
    if (id == "power") {
        power_ = std::clamp(static_cast<int>(std::lround(value)), geometry::Displacement::kMinPower,
                            geometry::Displacement::kMaxPower);
        session_.publishToolStatus();
        return true;
    }
    if (id == "elevation") {
        elevation_ = value;
        session_.publishToolStatus();
        return true;
    }
    return false;
}

bool DisplacementTool::perform(const ActionId& action) {
    if (action != "displacement.apply") return false;
    apply();
    return true;
}

bool DisplacementTool::apply() {
    if (!session_.document()) return false;
    if (session_.selection().mode() != SelectionMode::Faces) {
        setNotice("Select faces first (face selection mode)");
        return false;
    }
    const auto sides = displacementTargets(session_);
    Result<DisplacementEditReport> report;
    switch (mode_) {
    case Mode::Create: report = createDisplacements(session_, sides, power_, elevation_); break;
    case Mode::Subdivide: report = subdivideDisplacements(session_, sides); break;
    case Mode::Sew: report = sewDisplacements(session_, sides); break;
    }
    if (!report) {
        setNotice(report.error().what());
        return false;
    }
    setNotice(report->message);
    return report->changed != 0;
}

Handled DisplacementTool::input(const InputEvent& event, const ToolView& view) {
    if (!session_.document() || event.navigating) return false;
    if (event.kind == InputKind::KeyDown && !event.autoRepeat && event.key == "Enter") {
        apply();
        return true;
    }
    if (event.kind != InputKind::Press || event.button != PointerButton::Left) return false;
    const auto hit = session_.picks().first(view.cursorRay);
    auto& selection = session_.selection();
    selection.setMode(SelectionMode::Faces);
    if (hit && hit->side) {
        if (event.ctrl) selection.toggle(*hit->side);
        else if (event.shift) selection.add(*hit->side);
        else selection.select(*hit->side);
    } else if (!event.ctrl && !event.shift) selection.clear();
    notice_.clear();
    session_.publishToolStatus();
    return true;
}

ToolOverlay DisplacementTool::overlay(const ToolView&, InputSource) const { return {}; }

ToolReadout DisplacementTool::readout() const {
    ToolReadout readout;
    readout.badge = {"Displacement"};
    readout.detail = mode_ == Mode::Create ? "Create" : mode_ == Mode::Subdivide ? "Subdivide" : "Sew";
    readout.parameters.push_back({"mode", "Mode", ToolParameter::Kind::Choice, static_cast<double>(mode_), 0.0, 2.0,
                                  {"Create", "Subdivide", "Sew"}});
    if (mode_ == Mode::Create) {
        readout.parameters.push_back({"power", "Power", ToolParameter::Kind::Integer, double(power_),
                                      double(geometry::Displacement::kMinPower), double(geometry::Displacement::kMaxPower), {}});
        readout.parameters.push_back({"elevation", "Elevation", ToolParameter::Kind::Number, elevation_, -65536.0, 65536.0, {}});
    }
    const char* help = mode_ == Mode::Create
        ? "Click faces (Ctrl toggles, Shift adds), then Enter to make them displacements; quad faces only."
        : mode_ == Mode::Subdivide ? "Select displacements, then Enter to subdivide (smooth) them."
                                   : "Select neighbouring displacements, then Enter to sew their shared edges.";
    readout.status = notice_.empty() ? help : notice_ + " · " + help;
    return readout;
}

} // namespace apex::editor
