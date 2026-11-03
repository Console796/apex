#pragma once

// Command sources for the command palette (ui/command_palette.hpp). Each function registers one
// provider; to add a family of commands, write another and call it where the palette is built
// (MainWindow::buildCommandPalette).

#include "editor/actions.hpp"
#include "ui/command_palette.hpp"

#include <map>

class QAction;

namespace apex::editor {
class EditorSession;
}

namespace apex::ui {

class ViewportArea;

// Display mode, preset, projection, axis, overlays and A/B compare of the active viewport, plus
// "Show area" for every non-viewport area role.
void registerViewportCommands(CommandRegistry& registry, ViewportArea& area);

// One command per menu / toolbar QAction registered under an editor action id: the title, category,
// description and shortcut come from the action registry and the current binding preset, the
// enabled and checked state from the QAction. Nothing to maintain when an action is added.
void registerEditorActionCommands(CommandRegistry& registry, const std::map<editor::ActionId, QAction*>& actions,
                                  const editor::EditorSession& session);

}  // namespace apex::ui
