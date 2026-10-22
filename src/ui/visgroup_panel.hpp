#pragma once

#include <QTreeWidget>
#include "editor/editor_session.hpp"

namespace apex::ui {

class VisgroupPanel : public QTreeWidget {
    Q_OBJECT
public:
    explicit VisgroupPanel(QWidget* parent = nullptr);
    ~VisgroupPanel() override;
    void setDocument(document::Document* document);
    void setEditorSession(editor::EditorSession* session);
    void refresh();
    std::optional<int> currentVisgroup() const;
    bool newVisgroup(bool child = false);
    bool moveVisgroup(int id, std::optional<int> parent = {});

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    document::Document* document_ = nullptr;
    std::size_t documentObserver_ = 0;
    editor::EditorSession* session_ = nullptr;
    std::size_t sessionObserver_ = 0;
    bool refreshPending_ = false;
    bool attached() const;
    void scheduleRefresh();
    void updateActions();
};

} // namespace apex::ui
