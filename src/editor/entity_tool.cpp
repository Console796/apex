#include "editor/entity_tool.hpp"
#include "editor/editor_session.hpp"

namespace apex::editor {

Handled EntityTool::input(const InputEvent& event, const ToolView& view) {
    if (event.kind == InputKind::Leave) session_.clearHover();
    selectionGesture_.input(event, view, *this, session_);
    return false;
}
void EntityTool::pointerHover(Ray) { session_.clearHover(); }
void EntityTool::pointerClick(Ray ray, Modifiers, bool) {
    session_.placeEntity(session_.currentEntityClass(), ray, session_.alignToSurface());
}
ToolOverlay EntityTool::overlay(const ToolView&, InputSource source) const {
    ToolOverlay result;
    result.rectangle = selectionGesture_.rectangle(source);
    return result;
}
ToolReadout EntityTool::readout() const {
    return {{"Entity"}, session_.currentEntityClass(), {}, true};
}

} // namespace apex::editor
