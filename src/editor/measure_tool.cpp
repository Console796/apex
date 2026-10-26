#include "editor/measure_tool.hpp"
#include "editor/editor_session.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace apex::editor {
Measurement measure(DVec3 a, DVec3 b, std::optional<DVec3> normalA, std::optional<DVec3> normalB) {
    const auto finite = [](DVec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    if (!finite(a) || !finite(b)) throw std::invalid_argument("Measure requires finite points");
    Measurement result;
    result.delta = b - a;
    result.distance = glm::length(result.delta);
    if (normalA && normalB) {
        if (!finite(*normalA) || !finite(*normalB) || glm::length(*normalA) == 0 || glm::length(*normalB) == 0)
            throw std::invalid_argument("Measure requires nonzero finite normals");
        result.faceAngle = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(*normalA), glm::normalize(*normalB)), -1.0, 1.0)));
    }
    return result;
}
Handled MeasureTool::input(const InputEvent& e, const ToolView& view) {
    if (e.kind == InputKind::Cancel || (e.kind == InputKind::KeyDown && e.key == "Escape")) { cancel(); return true; }
    if (!session_.document() || e.navigating) return false;
    if (e.kind == InputKind::Release && e.button == PointerButton::Left) return true;
    if (e.kind != InputKind::Press || e.button != PointerButton::Left) return false;
    const auto snap = session_.snap(view.cursorRay, view, actionModifier(session_.preset(), e, "snap.suspend"));
    if (snap.kind == SnapKind::None) return true;
    std::optional<DVec3> normal;
    if (snap.side) if (auto f = faceSnapshot(*session_.document(), *snap.side)) normal = f->face.plane.normal;
    if (points_.size() == 2) points_.clear();
    points_.push_back({snap.point, normal});
    session_.notify();
    return true;
}
void MeasureTool::cancel() {
    if (points_.empty()) return;
    points_.clear(); session_.notify();
}
std::string MeasureTool::detail() const {
    if (points_.size() != 2) return "Place two points";
    const auto m = measure(points_[0].position, points_[1].position, points_[0].normal, points_[1].normal);
    std::ostringstream text;
    text << std::setprecision(6) << m.distance << " units; delta " << m.delta.x << ", " << m.delta.y << ", " << m.delta.z;
    if (m.faceAngle) text << "; face angle " << *m.faceAngle << " deg";
    return text.str();
}
ToolOverlay MeasureTool::overlay(const ToolView&, InputSource) const {
    ToolOverlay result;
    const auto colour = session_.overlayPalette().vertexSelected;
    for (const auto& p : points_) result.worldPoints.push_back({p.position, colour, 9});
    if (points_.size() == 2) {
        const auto a = points_[0].position, b = points_[1].position;
        result.worldSegments.push_back({a, b, colour, 2});
        auto label = detail();
        if (const auto separator = label.find("; "); separator != std::string::npos)
            label.replace(separator, 2, "\n");
        result.labels.push_back({(a + b) * 0.5, std::move(label), {8, -8}});
    }
    return result;
}
ToolReadout MeasureTool::readout() const { return {{"Measure"}, {}, detail()}; }
} // namespace apex::editor
