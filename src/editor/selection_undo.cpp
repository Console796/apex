#include "editor/selection_undo.hpp"

namespace apex::editor {

void recordSelection(document::Transaction& transaction, const Selection& before, const Selection& after) {
    transaction.setSelection(before.toBlob(), after.toBlob());
}

bool restoreFromUndo(document::Document& doc, Selection& selection) {
    if (!doc.restoredSelection() || !selection.fromBlob(*doc.restoredSelection())) return false;
    selection.prune(doc);
    return true;
}

} // namespace apex::editor
