#pragma once

#include "editor/pie_menu.hpp"

#include <string_view>

namespace apex::editor {
const PieMenu& toolPie();
// To Detail and To World with the grouping commands (plan 0.8.9 D16).
const PieMenu& objectPie();
// The pie a pie.* action opens, or nullptr.
const PieMenu* pieFor(std::string_view action);
} // namespace apex::editor
