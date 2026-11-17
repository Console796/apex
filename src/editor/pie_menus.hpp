#pragma once

#include "editor/pie_menu.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::editor {
class PieRegistry {
public:
    bool add(std::string action, PieMenu menu);
    const PieMenu* find(std::string_view action) const;
    void freeze() { frozen_ = true; }
    // Registered pies in registration order: the built-ins first (today's code pies, the defaults for
    // presets that name none).
    const std::vector<std::pair<std::string, PieMenu>>& entries() const { return entries_; }
private:
    std::vector<std::pair<std::string, PieMenu>> entries_;
    bool frozen_ = false;
};
PieRegistry& mutablePieRegistry();
const PieRegistry& pieRegistry();
const PieMenu& toolPie();
// To Detail and To World with the grouping commands (plan 0.8.9 D16).
const PieMenu& objectPie();
// The pie a pie.* action opens, or nullptr.
const PieMenu* pieFor(std::string_view action);
const PieMenu* pieFor(std::string_view action, const Preset& preset);

// ---- Pie layouts in presets (plan 0.9 D7) ----
// The layout a preset gives `action`: its own when it has one, else the built-in default (nullptr for an
// action that opens no pie).
PieMenu effectivePie(std::string_view action, const Preset& preset);
// Whether the preset carries its own layout for this pie (false means "the built-in default").
inline bool hasOwnPie(const Preset& preset, std::string_view action) { return preset.pies.contains(action); }
// A pie item whose action is not registered (a plugin that is absent or disabled) is dormant.
bool isDormantPieItem(const PieItem& item, const Actions& actions = actionRegistry());
// The menu a gesture shows: dormant items left out. The preset's layout is untouched, so the items come
// back when the plugin does.
PieMenu visiblePie(const PieMenu& menu, const Actions& actions = actionRegistry());
// What `parsePreset` rejects: more than eight items, an item with no slot, two items in one slot.
// Empty means the layout can be saved and loaded.
std::vector<std::string> validatePie(const PieMenu& menu);
// Puts `item` in `slot` (its own slot field is overwritten), replacing what was there.
void setPieSlot(PieMenu& menu, PieSlot slot, PieItem item);
// Empties the slot; false when it was already empty.
bool clearPieSlot(PieMenu& menu, PieSlot slot);
const PieItem* pieItemAt(const PieMenu& menu, PieSlot slot);
} // namespace apex::editor
