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

namespace apex::document {
class Document;
}

namespace apex::ui {

class LogPanel;
class OutlinerPanel;
class ViewportArea;
class WorkspaceBar;

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
    // Applies a registry theme by id and checks it in the View > Theme menu.
    void setThemeId(const QString& id);
    // Applies unsaved tokens (theme editor live preview).
    void previewTokens(const Tokens& tokens);
    // Rebuilds View > Theme from the theme registry (after custom themes changed).
    void refreshThemeMenu();
    // The first outliner/log area of the active workspace (null when it has none).
    OutlinerPanel* outliner() const;
    LogPanel* logPanel() const;
    ViewportArea* viewportArea() const { return viewport_; }
    WorkspaceBar* workspaceBar() const { return workspaces_; }
    // Preferences: whether Object properties opens the inspector in a floating window rather than
    // the workspace's inspector area.
    static bool inspectorInWindow();
    static void setInspectorInWindow(bool window);
    void showInspector();
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
    void setNewSolidStyle(std::string material, geometry::UvAlignment alignment);
    void openPreferences();

private slots:
    void openDialog();
    void save();
    void saveAs();

private:
    void buildMenus();
    void rebuildAddonsMenu();
    void reloadGameContent();
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
    void centerViewsOnSelection();
    void buildClipboardUi();
    void openPasteSpecial();
    void openTieDialog();
    QStringList solidClasses();
    void refreshClipboardState();
    void updateEditorActions();
    void updateGridStatus();
    void buildAreas();
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

    ViewportArea* viewport_ = nullptr;
    WorkspaceBar* workspaces_ = nullptr;
    QComboBox* entityClass_ = nullptr;
    QCheckBox* alignToSurface_ = nullptr;
    QComboBox* profileCombo_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QMenu* addonsMenu_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;
    QAction* uvAlignmentAction_ = nullptr;
    QMenu* themeMenu_ = nullptr;
    QLabel* gridLabel_ = nullptr;
    QLabel* planeLabel_ = nullptr;
    QLabel* readoutLabel_ = nullptr;
    QLabel* modeLabel_ = nullptr;
    QLabel* cameraLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    QLabel* gpuLabel_ = nullptr;
};

}  // namespace apex::ui
