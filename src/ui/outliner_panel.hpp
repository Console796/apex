#pragma once

#include <QTreeWidget>
class QLineEdit;
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

signals:
    void instanceCreateRequested();
protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    bool dropMimeData(QTreeWidgetItem* parent, int index, const QMimeData* data, Qt::DropAction action) override;
    void startDrag(Qt::DropActions actions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void applyFilter();
    bool visibilityClick_ = false;
    QLineEdit* filter_ = nullptr;
    document::Document* document_ = nullptr;
    std::size_t documentObserver_ = 0;
    editor::EditorSession* session_ = nullptr;
    std::size_t sessionObserver_ = 0;
    std::optional<std::uint64_t> syncedSelection_;
};

}  // namespace apex::ui
