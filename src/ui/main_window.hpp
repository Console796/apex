#pragma once

#include <QMainWindow>
#include <QString>
#include <memory>
#include <vector>

#include "assets/game_profile.hpp"
#include "editor/editor_session.hpp"
#include "ui/theme.hpp"

class QAction;
class QComboBox;
class QLabel;
class QMenu;

namespace ads {
class CDockManager;
}

namespace apex::document {
class Document;
}

namespace apex::ui {

class LogPanel;
class OutlinerPanel;
class ViewportArea;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Profiles offered by the picker; the previously selected name is kept if still present.
    void setProfiles(std::vector<assets::GameProfile> profiles);
    // Loads user profiles (<config>/profiles/*.json) plus the detected P2CE install.
    void reloadProfiles();
    const assets::GameProfile* currentProfile() const;

    bool openFile(const QString& path);
    bool saveFile(const QString& path);
    editor::EditorSession& editorSession() { return session_; }
    document::Document* document() const { return document_.get(); }
    const QString& documentPath() const { return path_; }

    void setTheme(Theme theme);
    OutlinerPanel* outliner() const { return outliner_; }
    LogPanel* logPanel() const { return log_; }
    ViewportArea* viewportArea() const { return viewport_; }
    QComboBox* profilePicker() const { return profileCombo_; }
    QStringList recentFiles() const;

private slots:
    void openDialog();
    void save();
    void saveAs();

private:
    void buildMenus();
    void updateEditorActions();
    void updateGridStatus();
    void applyPreset(const editor::Preset& preset);
    void buildDocks();
    void buildStatusBar();
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void updateTitle();
    void restoreLayout();
    void closeEvent(QCloseEvent* event) override;

    std::unique_ptr<document::Document> document_;
    editor::EditorSession session_;
    std::size_t sessionObserver_ = 0;
    std::optional<bool> savedMarqueeTouching_;
    std::map<editor::ActionId, QAction*> editorActions_;
    QString path_;
    std::vector<assets::GameProfile> profiles_;

    ads::CDockManager* dockManager_ = nullptr;     // Left: outliner.
    ads::CDockManager* logDockManager_ = nullptr;  // Bottom: log.
    ViewportArea* viewport_ = nullptr;
    OutlinerPanel* outliner_ = nullptr;
    LogPanel* log_ = nullptr;
    QComboBox* profileCombo_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;
    QAction* darkAction_ = nullptr;
    QAction* lightAction_ = nullptr;
    QLabel* gridLabel_ = nullptr;
    QLabel* planeLabel_ = nullptr;
    QLabel* cameraLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    QLabel* gpuLabel_ = nullptr;
};

}  // namespace apex::ui
