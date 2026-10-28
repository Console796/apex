#include "editor/entity_tool.hpp"
#include "editor/editor_session.hpp"

namespace apex::editor {

Handled EntityTool::input(const InputEvent& event, const ToolView& view) {
    if (event.kind == InputKind::Leave) {
        session_.clearHover();
        clearGhost();
    }
    selectionGesture_.input(event, view, *this, session_);
    return false;
}
// A model or prefab item draws its box where a click would put it, from the same plan the click
// places with (0.3 Bite 6). A class item, or a ray that finds no surface, draws nothing.
void EntityTool::pointerHover(Ray ray) {
    session_.clearHover();
    const auto item = session_.placementItem();
    if (std::holds_alternative<ClassItem>(item)) return clearGhost();
    const auto plan = session_.planPlacement(item, ray, session_.alignToSurface());
    auto box = plan ? placedBox(plan->placement, plan->bounds) : Result<geometry::ConvexPolyhedron>{std::unexpected(plan.error())};
    if (!box) return clearGhost();
    const auto before = preview_.revision();
    preview_.setGhosts({{std::move(*box), {}, 0.35f}});
    if (preview_.revision() != before) session_.notify();
}
void EntityTool::clearGhost() {
    const auto before = preview_.revision();
    preview_.clear();
    if (preview_.revision() != before) session_.notify();
}
void EntityTool::pointerClick(Ray ray, Modifiers, bool) {
    session_.placeItem(session_.placementItem(), ray, session_.alignToSurface());
}
ToolOverlay EntityTool::overlay(const ToolView&, InputSource source) const {
    ToolOverlay result;
    result.rectangle = selectionGesture_.rectangle(source);
    return result;
}
ToolReadout EntityTool::readout() const {
    if (const auto& prefab = session_.placementPrefab()) {
        const auto name = prefab->path.substr(prefab->path.find_last_of("/\\") + 1);
        return {{"Entity"}, "Prefab (" + name + ")", "Entity: prefab " + name, true};
    }
    if (!session_.placementModel().empty()) {
        const auto& model = session_.placementModel();
        const auto name = model.substr(model.find_last_of('/') + 1);
        const auto detail = session_.modelPlacementClass() + " (" + name + ")";
        return {{"Entity"}, detail, "Entity: " + detail, true};
    }
    return {{"Entity"}, session_.currentEntityClass(), {}, true};
}

} // namespace apex::editor
