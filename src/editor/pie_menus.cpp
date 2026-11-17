#include "editor/pie_menus.hpp"

namespace apex::editor {
const PieMenu& toolPie() {
    static const PieMenu menu{"Tools", {
        {"tool.clip", PieSlot::NW, "Clip"},
        {"tool.vertex", PieSlot::N, "Vertex"},
        {"tool.primitive", PieSlot::NE, "Primitive"},
        {"tool.select", PieSlot::W, "Select"},
        {"tool.block", PieSlot::E, "Block"},
        {"tool.measure", PieSlot::SW, "Measure"},
        {"tool.entity", PieSlot::S, "Entity"},
        {"tool.apply", PieSlot::SE, "Apply"},
    }};
    return menu;
}
const PieMenu& objectPie() {
    static const PieMenu menu{"Objects", {
        {"edit.to_detail", PieSlot::N, "To Detail"},
        {"edit.group", PieSlot::W, "Group"},
        {"edit.ungroup", PieSlot::E, "Ungroup"},
        {"edit.to_world", PieSlot::S, "To World"},
    }};
    return menu;
}
const PieMenu* pieFor(std::string_view action) {
    if (action == "pie.tools") return &toolPie();
    if (action == "pie.objects") return &objectPie();
    return nullptr;
}
} // namespace apex::editor
