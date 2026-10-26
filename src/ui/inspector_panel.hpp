#pragma once

#include "editor/editor_session.hpp"
#include "ui/rename_confirmation.hpp"
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QVBoxLayout;
class QTabWidget;
class QTableWidget;
class QKeyEvent;

namespace apex::ui {

class InspectorPanel : public QWidget {
    Q_OBJECT
public:
    explicit InspectorPanel(QWidget* parent = nullptr);
    ~InspectorPanel() override;
    void setDocument(document::Document* document);
    void setEditorSession(editor::EditorSession* session);
    void setMaterialBrowser(std::function<void(QString, QString, std::function<void(QString)>)> browser);
    void setRenameDialogPresenter(RenameDialogPresenter presenter) { renamePresenter_ = std::move(presenter); }
    void refresh();

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    void requestRefresh();
    bool current(const std::vector<document::NodeId>& entities) const;
    void watch(QWidget* field);
    void refreshConnectionTabs();
    void handleConnectionKey(QKeyEvent* event);
    document::Document* document_ = nullptr;
    editor::EditorSession* session_ = nullptr;
    std::size_t documentObserver_ = 0, sessionObserver_ = 0;
    QComboBox* classPicker_ = nullptr;
    QCheckBox* raw_ = nullptr;
    QWidget* rows_ = nullptr;
    QVBoxLayout* layout_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QWidget* properties_ = nullptr;
    QVBoxLayout* propertiesLayout_ = nullptr;
    QTableWidget* outputs_ = nullptr;
    QTableWidget* inputs_ = nullptr;
    std::vector<document::NodeId> entities_;
    std::uint64_t selectionRevision_ = 0, classesRevision_ = 0;
    bool queued_ = false, rebuilding_ = false;
    std::function<void(QString, QString, std::function<void(QString)>)> materialBrowser_;
    RenameDialogPresenter renamePresenter_;
    std::optional<std::size_t> pendingOutputEdit_;
};

} // namespace apex::ui
