#pragma once

#include <QWidget>
#include "editor/editor_session.hpp"

class QLabel;
class QListWidget;

namespace apex::ui {

class HistoryPanel : public QWidget {
    Q_OBJECT
public:
    explicit HistoryPanel(QWidget* parent = nullptr);
    ~HistoryPanel() override;
    void setDocument(document::Document* document);
    void setEditorSession(editor::EditorSession* session);
    void refresh();
    QListWidget* list() const { return list_; }
    QLabel* memoryLabel() const { return memory_; }

private:
    document::Document* document_ = nullptr;
    std::size_t documentObserver_ = 0;
    editor::EditorSession* session_ = nullptr;
    std::size_t sessionObserver_ = 0;
    QListWidget* list_ = nullptr;
    QLabel* memory_ = nullptr;
};

} // namespace apex::ui
