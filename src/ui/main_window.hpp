#pragma once

#include <QMainWindow>
#include <QString>
#include <memory>
#include <vector>

#include "assets/game_profile.hpp"
#include "editor/editor_session.hpp"
#include "editor/autosave.hpp"
#include "editor/preset_io.hpp"
#include "ui/theme.hpp"

class QAction;
class QComboBox;
class QCheckBox;
class QLabel;
class QMenu;
class QToolButton;
class QTimer;

namespace ads {
class CDockManager;
}

namespace apex::document {
class Document;
}

namespace apex::ui {

class LogPanel;
class OutlinerPanel;
class VisgroupPanel;
class HistoryPanel;
class InspectorPanel;
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
    void newFile();
    bool recoverAutosave(const editor::Recovery& recovery);
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

    // Clipboard (plan 0.2.6, D12): copies carry the VMF fragment under this MIME type and as
    // plain text, so other Apex instances and text editors can paste it.
    static QString fragmentMimeType();
    // The VMF fragment on the system clipboard: the custom MIME type first, then plain text.
    // Empty when neither parses as a fragment.
    static std::string clipboardFragment();
    // Where Paste lands: at the pointer's work-plane hit, or in place without a pointer/hit.
    editor::PasteOptions pastePlacement(bool inPlace) const;
    // Handles the edit.* clipboard, duplicate, group and tie actions (menus and shortcuts).
    bool performEditAction(const editor::ActionId& id);

    // Settings (the Preferences dialog drives these; each persists its own value).
    const editor::PresetStore& presetStore() const { return presetStore_; }
    // Makes the preset the active keymap, rebinds the menu shortcuts and remembers its name.
    void applyPreset(const editor::Preset& preset);
    // Rebuilds Settings > Binding preset after user presets were saved or deleted.
    void refreshPresetMenu();
    void setAutosave(bool enabled, int intervalMinutes);
    void setUndoCapMiB(int mib);
    void openPreferences();

private slots:
    void openDialog();
    void save();
    void saveAs();

private:
    void buildMenus();
    bool loadFile(const QString& readPath, const QString& source, bool recovered);
    void installDocument(std::unique_ptr<document::Document> doc, const vmf::VmfFile& parsed,
                         const QString& path, bool unsaved);
    void startAutosave();
    void snapshotAutosave();
    void configureAutosave();
    void offerRecovery();
    void buildTransformUi();
    void buildEntityUi();
    void loadEntityClasses();
    void refreshEntityUi();
    void openTransformDialog();
    void buildClipboardUi();
    void openPasteSpecial();
    void openTieDialog();
    QStringList solidClasses();
    void refreshClipboardState();
    void updateEditorActions();
    void updateGridStatus();
    void buildDocks();
    void buildStatusBar();
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void updateTitle();
    void restoreLayout();
    void closeEvent(QCloseEvent* event) override;

    std::unique_ptr<document::Document> document_;
    editor::EditorSession session_;
    std::unique_ptr<editor::Autosave> autosave_;
    QTimer* autosaveTimer_ = nullptr;
    std::uint64_t savedState_ = 0;
    bool unsaved_ = false;
    std::size_t sessionObserver_ = 0;
    bool clipboardHasFragment_ = false;
    std::optional<std::uint64_t> shownClassesRevision_;
    std::optional<bool> savedMarqueeTouching_;
    std::map<editor::ActionId, QAction*> editorActions_;
    editor::PresetStore presetStore_;
    QMenu* presetMenu_ = nullptr;
    QString path_;
    std::vector<assets::GameProfile> profiles_;

    ads::CDockManager* dockManager_ = nullptr;     // Left: outliner.
    ads::CDockManager* logDockManager_ = nullptr;  // Bottom: log.
    ViewportArea* viewport_ = nullptr;
    OutlinerPanel* outliner_ = nullptr;
    VisgroupPanel* visgroups_ = nullptr;
    HistoryPanel* history_ = nullptr;
    InspectorPanel* inspector_ = nullptr;
    QComboBox* entityClass_ = nullptr;
    QCheckBox* alignToSurface_ = nullptr;
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
    QLabel* readoutLabel_ = nullptr;
    QLabel* cameraLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    QLabel* gpuLabel_ = nullptr;
};

}  // namespace apex::ui
