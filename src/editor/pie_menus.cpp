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
bool PieRegistry::add(std::string action, PieMenu menu) {
    if (frozen_ || action.empty() || find(action)) return false;
    entries_.emplace_back(std::move(action), std::move(menu));
    return true;
}
const PieMenu* PieRegistry::find(std::string_view action) const {
    for (const auto& [id, menu] : entries_) if (id == action) return &menu;
    return nullptr;
}
PieRegistry& mutablePieRegistry() {
    static PieRegistry registry = [] {
        PieRegistry r;
        r.add("pie.tools", toolPie());
        r.add("pie.objects", objectPie());
        return r;
    }();
    return registry;
}
const PieRegistry& pieRegistry() { return mutablePieRegistry(); }
const PieMenu* pieFor(std::string_view action) {
    const auto* menu = pieRegistry().find(action);
    if (!menu) return nullptr;
    // Preserve the long-standing identities exposed by the built-in accessors.
    if (action == "pie.tools") return &toolPie();
    if (action == "pie.objects") return &objectPie();
    return menu;
}
const PieMenu* pieFor(std::string_view action, const Preset& preset) {
    const auto it = preset.pies.find(action);
    return it == preset.pies.end() ? pieFor(action) : &it->second;
}
} // namespace apex::editor
