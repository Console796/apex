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
} // namespace apex::editor
