#pragma once

#include "editor/editor_session.hpp"
#include "ui/rename_confirmation.hpp"
#include <QWidget>
#include <functional>
#include <array>

class QCheckBox;
class QMenu;
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
    void setAssetBrowser(std::function<void(QString, QString, QString, std::function<void(QString)>)> browser);
    void setMaterialBrowser(std::function<void(QString, QString, std::function<void(QString)>)> browser);
    void setOpenInstanceHandler(std::function<bool(document::NodeId)> resolved,
                                std::function<void(document::NodeId)> open) {
        instanceResolved_ = std::move(resolved); openInstance_ = std::move(open); requestRefresh();
    }
    void setRenameDialogPresenter(RenameDialogPresenter presenter) { renamePresenter_ = std::move(presenter); }
    // Tests replace the blocking menu.exec(); the hook receives the populated menu.
    void setConnectionMenuHook(std::function<void(QMenu*)> hook) { menuHook_ = std::move(hook); }
    void refresh();

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    void requestRefresh();
    bool current(const std::vector<document::NodeId>& entities) const;
    void watch(QWidget* field);
    void refreshConnectionTabs(bool refreshInputs = false);
    bool connectionRowMatches(int row, int editedColumn = -1) const;
    void handleConnectionKey(QKeyEvent* event);
    void addConnectionRow();
    void deleteConnectionRows();
    void duplicateConnectionRow();
    void copyConnectionRows();
    void pasteConnectionRows();
    void showConnectionMenu(const QPoint& position);
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
    bool queued_ = false, rebuilding_ = false, colorDialogOpen_ = false;
    std::function<void(QString, QString, std::function<void(QString)>)> materialBrowser_;
    std::function<void(QString, QString, QString, std::function<void(QString)>)> assetBrowser_;
    std::function<bool(document::NodeId)> instanceResolved_;
    std::function<void(document::NodeId)> openInstance_;
    RenameDialogPresenter renamePresenter_;
    std::function<void(QMenu*)> menuHook_;
    std::optional<std::size_t> pendingOutputEdit_;
    std::vector<std::array<QString, 6>> displayedConnections_;
};

} // namespace apex::ui
