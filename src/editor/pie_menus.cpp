#include "editor/pie_menus.hpp"

#include <algorithm>
#include <array>

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

PieMenu effectivePie(std::string_view action, const Preset& preset) {
    const auto* menu = pieFor(action, preset);
    return menu ? *menu : PieMenu{};
}
bool isDormantPieItem(const PieItem& item, const Actions& actions) { return !actions.find(item.action); }
PieMenu visiblePie(const PieMenu& menu, const Actions& actions) {
    PieMenu result{menu.title, {}};
    for (const auto& item : menu.items)
        if (!isDormantPieItem(item, actions)) result.items.push_back(item);
    return result;
}
std::vector<std::string> validatePie(const PieMenu& menu) {
    std::vector<std::string> problems;
    if (menu.items.size() > 8) problems.push_back("Pie has more than eight items");
    std::array<bool, 8> used{};
    for (const auto& item : menu.items) {
        if (item.action.empty()) problems.push_back("Pie item has no action");
        if (!item.slot) { problems.push_back("Missing pie slot for " + item.action); continue; }
        const auto index = static_cast<std::size_t>(*item.slot);
        if (index >= used.size()) { problems.push_back("Invalid pie slot for " + item.action); continue; }
        if (used[index]) problems.push_back("Duplicate pie slot for " + item.action);
        used[index] = true;
    }
    return problems;
}
const PieItem* pieItemAt(const PieMenu& menu, PieSlot slot) {
    for (const auto& item : menu.items) if (item.slot == slot) return &item;
    return nullptr;
}
void setPieSlot(PieMenu& menu, PieSlot slot, PieItem item) {
    item.slot = slot;
    clearPieSlot(menu, slot);
    menu.items.push_back(std::move(item));
}
bool clearPieSlot(PieMenu& menu, PieSlot slot) {
    return std::erase_if(menu.items, [&](const PieItem& item) { return item.slot == slot; }) > 0;
}
} // namespace apex::editor
