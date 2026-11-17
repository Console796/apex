#include "editor/rope_tool.hpp"

#include "core/strings.hpp"
#include "editor/editor_session.hpp"
#include "editor/entity_defaults.hpp"
#include "editor/keyframe_chain.hpp"

#include <iomanip>
#include <sstream>

namespace apex::editor {

void RopeTool::activate() { session_.selection().setMode(SelectionMode::Objects); }

void RopeTool::cancel() {
    if (points_.empty() && notice_.empty()) return;
    points_.clear();
    notice_.clear();
    session_.publishToolStatus();
}

void RopeTool::commit() {
    if (points_.size() < 2) {
        notice_ = "Place at least two points";
        session_.publishToolStatus();
        return;
    }
    std::set<std::string> taken;
    for (const auto& name : session_.names().names()) taken.insert(toLowerAscii(name));
    std::vector<std::string> names;
    std::string previous = "rope_0";
    for (std::size_t i = 0; i < points_.size(); ++i) {
        previous = nextKeyframeName(previous, taken);
        names.push_back(previous);
    }
    // runEdit notifies documentChanged synchronously. Move the draft out first.
    auto points = std::move(points_);
    points_.clear();
    const auto done = session_.runEdit("Create rope", EditorSession::AfterEdit::Computed,
        [&](document::Transaction& tx) -> Result<EditedObjects> {
            EditedObjects result;
            for (std::size_t i = 0; i < points.size(); ++i) {
                auto block = kv1::Node::block("entity");
                const std::string classname = i == 0 ? "move_rope" : "keyframe_rope";
                block.append(kv1::Node::value("classname", classname)).value();
                block.append(kv1::Node::value("targetname", names[i])).value();
                std::ostringstream origin;
                origin << std::setprecision(17) << points[i].x << ' ' << points[i].y << ' ' << points[i].z;
                block.append(kv1::Node::value("origin", origin.str())).value();
                // The terminal key is explicitly empty, including when an FGD supplies a default.
                block.append(kv1::Node::value("NextKey", i + 1 < names.size() ? names[i + 1] : "")).value();
                if (const auto* cls = session_.entityClasses().find(classname))
                    for (const auto& [key, value] : missingEntityDefaults(block, *cls))
                        block.append(kv1::Node::value(key, value)).value();
                if (!block.find("Width")) block.append(kv1::Node::value("Width", "2")).value();
                if (!block.find("RopeMaterial")) block.append(kv1::Node::value("RopeMaterial", "cable/cable")).value();
                const auto handle = tx.create(std::move(block));
                result.items.push_back({session_.document()->get(handle)->id, EditedObjects::Item::Kind::Entity, {}, {}});
            }
            return result;
        });
    if (!done) { points_ = std::move(points); notice_ = done.error().what(); }
    else notice_ = "Created rope";
    session_.publishToolStatus();
}

Handled RopeTool::input(const InputEvent& event, const ToolView& view) {
    if (event.kind == InputKind::Cancel || (event.kind == InputKind::KeyDown && event.key == "Escape")) {
        cancel();
        return true;
    }
    if (!session_.document() || event.navigating) return false;
    if (event.kind == InputKind::KeyDown && !event.autoRepeat) {
        if (event.key == "Enter") { commit(); return true; }
        if (event.key == "Backspace") {
            if (!points_.empty()) points_.pop_back();
            session_.publishToolStatus();
            return true;
        }
    }
    if (event.kind == InputKind::Release && event.button == PointerButton::Left) return true;
    if (event.kind != InputKind::Press || event.button != PointerButton::Left) return false;
    const auto snap = session_.snap(view.cursorRay, view, actionModifier(session_.preset(), event, "snap.suspend"));
    if (snap.kind != SnapKind::None && (points_.empty() || glm::length(points_.back() - snap.point) > 1e-9)) {
        points_.push_back(snap.point);
        notice_.clear();
        session_.publishToolStatus();
    }
    return true;
}

ToolOverlay RopeTool::overlay(const ToolView&, InputSource) const {
    ToolOverlay result;
    const auto colour = session_.overlayPalette().vertexSelected;
    for (std::size_t i = 0; i < points_.size(); ++i) {
        result.worldPoints.push_back({points_[i], colour, 9});
        if (i) result.worldSegments.push_back({points_[i - 1], points_[i], colour, 2});
    }
    return result;
}

ToolReadout RopeTool::readout() const {
    return {{"Rope"}, std::to_string(points_.size()) + " points",
            notice_ + " Click to add points; Enter creates the rope; Backspace removes the last point; Esc cancels."};
}

} // namespace apex::editor
