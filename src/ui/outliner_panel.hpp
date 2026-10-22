#pragma once

#include <QTreeWidget>
#include "editor/editor_session.hpp"

namespace apex::document {
class Document;
}

namespace apex::ui {

// Selectable document tree; row IDs stay stable across document undo/redo.
class OutlinerPanel : public QTreeWidget {
    Q_OBJECT
public:
    explicit OutlinerPanel(QWidget* parent = nullptr);
    ~OutlinerPanel() override;
    void setEditorSession(editor::EditorSession* session);
    void syncSelection();

    void setDocument(document::Document* document);
    void refresh();

private:
    document::Document* document_ = nullptr;
    std::size_t documentObserver_ = 0;
    editor::EditorSession* session_ = nullptr;
    std::size_t sessionObserver_ = 0;
    std::optional<std::uint64_t> syncedSelection_;
};

}  // namespace apex::ui
