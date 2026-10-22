#pragma once

#include "editor/selection.hpp"

namespace apex::editor {

void recordSelection(document::Transaction& transaction, const Selection& before, const Selection& after);
// Call after a successful Document::undo()/redo(). Returns false for absent/invalid
// metadata; otherwise restores mode, policy, order and active target, then drops dead IDs.
bool restoreFromUndo(document::Document& document, Selection& selection);

} // namespace apex::editor
