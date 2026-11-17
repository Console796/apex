#pragma once

#include "editor/pie_menu.hpp"

#include <string_view>

namespace apex::editor {
class PieRegistry {
public:
    bool add(std::string action, PieMenu menu);
    const PieMenu* find(std::string_view action) const;
    void freeze() { frozen_ = true; }
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
} // namespace apex::editor
