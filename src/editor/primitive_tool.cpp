#include "editor/primitive_tool.hpp"

#include "editor/editor_session.hpp"
#include "editor/group_ops.hpp"
#include "geometry/solid_block.hpp"

#include <algorithm>
#include <cmath>

namespace apex::editor {
std::vector<geometry::ConvexPolyhedron> PrimitiveTool::shapes(const Gesture& g) const {
    if (!g.poly) return {};
    geometry::PrimitiveBox box;
    box.origin = g.plane.origin;
    box.axes = {g.plane.uAxis, g.plane.vAxis, g.plane.normal};
    box.bounds = {{glm::min(g.start, g.end), std::min(0.0, g.height)},
                  {glm::max(g.start, g.end), std::max(0.0, g.height)}};
    auto generated = geometry::primitive(box, parameters_);
    return generated ? std::move(*generated) : std::vector<geometry::ConvexPolyhedron>{};
}
void PrimitiveTool::gestureStarted() {
    lastGesture_.reset();
    lastNodes_.clear();
    lastStyle_.reset();
}
void PrimitiveTool::documentChanged() {
    if (!writing_) gestureStarted();
}
void PrimitiveTool::cancel() {
    gestureStarted();
    DrawExtrudeTool::cancel();
}
void PrimitiveTool::write(const std::vector<geometry::ConvexPolyhedron>& solids) {
    writing_ = true;
    session_.edit("Create primitive", [&](document::Transaction& tx) {
        for (const auto node : lastNodes_)
            if (const auto handle = session_.document()->handle(node)) tx.destroy(*handle);
        lastNodes_.clear();
        std::optional<document::NodeHandle> group;
        std::optional<int> groupId;
        if (solids.size() > 1) {
            auto block = kv1::Node::block("group");
            block.append(kv1::Node::value("id", "0")).value();
            group = tx.create(std::move(block), worldHandle(*session_.document()));
            const auto node = session_.document()->get(*group)->id;
            lastNodes_.push_back(node);
            groupId = vmfId(*session_.document(), node);
            tx.setEditor(*group, document::EditorEdit::setColor(groupColor(*groupId)));
        }
        std::vector<Target> selected;
        for (const auto& poly : solids) {
            const auto handle = tx.create(geometry::solidBlock(poly, *lastStyle_));
            const auto node = session_.document()->get(handle)->id;
            lastNodes_.push_back(node);
            selected.emplace_back(node);
            if (groupId) tx.setEditor(handle, document::EditorEdit::setGroup(groupId));
        }
        session_.selection().setMode(group ? SelectionMode::Groups : SelectionMode::Solids);
        if (group) selected = {session_.document()->get(*group)->id};
        session_.selection().replace(selected);
    }, coalesceKey_);
    writing_ = false;
}
bool PrimitiveTool::commitShape(const Gesture& g) {
    auto solids = shapes(g);
    if (solids.empty()) return false;
    gestureStarted();
    coalesceKey_ = "primitive:" + std::to_string(++operation_);
    lastStyle_ = session_.newSolidStyle();
    write(solids);
    lastGesture_ = g;
    return true;
}
ToolReadout PrimitiveTool::readout() const {
    auto result = DrawExtrudeTool::readout();
    result.badge = {"Primitive"};
    for (const auto& [source, gesture] : gestures_) {
        if (gesture.poly && shapes(gesture).empty()) {
            result.status = "Parameters do not fit the drawn box";
            result.warn = true;
        }
    }
    using Kind = ToolParameter::Kind;
    result.parameters.push_back({"type", "Type", Kind::Choice, static_cast<double>(parameters_.type), 0, 4,
                                 {"Wedge", "Cylinder", "Spike", "Arch", "Sphere"}});
    if (parameters_.type == geometry::PrimitiveType::Cylinder || parameters_.type == geometry::PrimitiveType::Spike ||
        parameters_.type == geometry::PrimitiveType::Arch)
        result.parameters.push_back({"sides", "Sides", Kind::Integer, static_cast<double>(parameters_.sides), 3,
                                     parameters_.type == geometry::PrimitiveType::Arch ? 100.0 : 64.0, {}});
    if (parameters_.type == geometry::PrimitiveType::Arch) {
        result.parameters.push_back({"arc", "Arc", Kind::Number, parameters_.arcAngle, 8, 360, {}});
        result.parameters.push_back({"start", "Start", Kind::Number, parameters_.startAngle, 0, 360, {}});
        result.parameters.push_back({"width", "Wall width", Kind::Number, parameters_.wallWidth, 0.01, 65536, {}});
        result.parameters.push_back({"add_height", "Add height", Kind::Number, parameters_.addHeight, -65536, 65536, {}});
    }
    if (parameters_.type == geometry::PrimitiveType::Sphere)
        result.parameters.push_back({"segments", "Segments", Kind::Integer, static_cast<double>(parameters_.segments), 3, 64, {}});
    return result;
}
bool PrimitiveTool::setParameter(std::string_view id, double value) {
    if (!std::isfinite(value)) return false;
    const auto fields = readout().parameters;
    const auto field = std::ranges::find(fields, id, &ToolParameter::id);
    if (field == fields.end() || value < field->minimum || value > field->maximum ||
        (field->kind != ToolParameter::Kind::Number && value != std::floor(value))) return false;
    const auto before = parameters_;
    if (id == "type") {
        parameters_.type = static_cast<geometry::PrimitiveType>(static_cast<int>(value));
        if (parameters_.type != geometry::PrimitiveType::Arch) parameters_.sides = std::min(parameters_.sides, 64);
    } else if (id == "sides") parameters_.sides = static_cast<int>(value);
    else if (id == "segments") parameters_.segments = static_cast<int>(value);
    else if (id == "arc") parameters_.arcAngle = value;
    else if (id == "start") parameters_.startAngle = value;
    else if (id == "width") parameters_.wallWidth = value;
    else if (id == "add_height") parameters_.addHeight = value;
    if (lastGesture_) {
        const auto solids = shapes(*lastGesture_);
        if (solids.empty()) { parameters_ = before; return false; }
        write(solids);
    }
    publish();
    return true;
}
} // namespace apex::editor
