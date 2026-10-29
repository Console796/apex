#include "ui/main_window.hpp"
#include "ui/compile_panel.hpp"
#include "ui/game_content.hpp"


#include <QAction>
#include <QWidgetAction>
#include <QSvgRenderer>
#include <QPainter>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QComboBox>
#include <QCheckBox>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFileDialog>
#include <QKeySequence>
#include <QInputDialog>
#include <QImage>
#include <QIcon>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QProcess>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <array>
#include <ranges>
#include <QCheckBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QDialog>
#include <QCoreApplication>
#include <QFormLayout>
#include "ui/recovery_dialog.hpp"
#include "ui/settings_dialog.hpp"
#include <filesystem>

#include <algorithm>
#include <tuple>

#include "assets/addons.hpp"
#include "assets/fgd.hpp"
#include "core/kv1.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/version.hpp"
#include "document/document.hpp"
#include "document/vmf.hpp"
#include "editor/vertex_tool.hpp"
#include "ui/log_panel.hpp"
#include "ui/key_chord.hpp"
#include "ui/pie_menu_popup.hpp"
#include "ui/transform_input.hpp"
#include "ui/input_log.hpp"
#include "ui/outliner_panel.hpp"
#include "ui/visgroup_panel.hpp"
#include "ui/history_panel.hpp"
#include "ui/inspector_panel.hpp"
#include "ui/paste_special_dialog.hpp"
#include "ui/tie_dialog.hpp"
#include "ui/prefab_dialog.hpp"
#include "editor/prefab.hpp"
#include "ui/transform_dialog.hpp"
#include "ui/viewport_area.hpp"
#include "ui/game_content.hpp"
#include "ui/materials_panel.hpp"
#include "ui/asset_browser_panel.hpp"
#include "ui/models_panel.hpp"
#include "ui/workspace_bar.hpp"
#include "ui/area_frame.hpp"
#include "ui/viewport_pane.hpp"
#include "viewport/map_views.hpp"

// Q_INIT_RESOURCE must be called outside a namespace; apex_ui is a static library.
static void initializeIcons() { Q_INIT_RESOURCE(icons); }

namespace apex::ui {
namespace {
constexpr std::string_view kToolActions[] = {"tool.select", "tool.entity", "tool.block", "tool.clip",
    "tool.vertex", "tool.primitive", "tool.apply", "tool.measure"};
}

namespace {
int undoCapMiB() {
    bool valid = false;
    const auto value = QSettings().value(QStringLiteral("history/undoCapMiB"), 512).toLongLong(&valid);
    return valid ? static_cast<int>(std::clamp<qlonglong>(value, 16, 8192)) : 512;
}

constexpr int kMaxRecent = 10;

QString fromStd(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

bool isTransformAction(const editor::ActionId& id) {
    return id.starts_with("gizmo.") || id.starts_with("nudge.") || id.starts_with("edit.flip.") ||
           id == "transform.dialog" || id == "transform.texture_lock.toggle" || id == "snap.mode.toggle" ||
           id == "view.model_overlay.toggle";
}

// Actions whose plain (unmodified) default keys belong to the focused pane.
bool isPaneKeyAction(const editor::ActionId& id) {
    return isTransformAction(id) ||
           (id.starts_with("view.") && id != "view.properties" && id != "view.center_selection") ||
           id == "edit.delete" || id == "edit.duplicate" || id.starts_with("selection.mode.") ||
           id.starts_with("vertex.mode.") || id == "pie.tools";
}

bool looksLikeFragment(std::string_view text) {
    if (text.empty() || text.size() > (256u << 20)) return false;
    auto parsed = kv1::parse(std::string(text));
    if (!parsed) return false;
    for (const auto& child : parsed->root().children()) {
        if (child.kind() != kv1::NodeKind::Block) continue;
        if (ciEquals(child.key(), "group") || ciEquals(child.key(), "solid") || ciEquals(child.key(), "entity"))
            return true;
    }
    return false;
}

QString profileDir() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/profiles");
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      presetStore_(std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toStdString()) /
                   "keymaps") {
    resize(1280, 800);
    LogPanel::startCapture();
    gameContent_ = std::make_unique<GameContent>();
    gameContent_->setPrefabUserFolder(std::filesystem::path(QSettings().value(QStringLiteral("prefabs/folder")).toString().toStdString()));
    const auto material = QSettings().value(QStringLiteral("editor/newSolidMaterial"),
                                           QStringLiteral("dev/dev_measuregeneric01")).toString();
    const bool worldUv = QSettings().value(QStringLiteral("editor/newSolidWorldUv"), false).toBool();
    const int extrapolation = std::clamp(QSettings().value(QStringLiteral("editor/faceExtrapolation"), 0).toInt(), 0, 2);
    QPointer<GameContent> content(gameContent_.get());
    session_.faceTextureSize = [content](std::string_view name) {
        return content ? content->faceTextureSize(name) : std::pair{256, 256};
    };
    session_.setNewSolidMaterial(material.toStdString());
    lastActiveMaterial_ = material.toStdString();
    session_.setNewSolidUvAlignment(worldUv ? geometry::UvAlignment::World : geometry::UvAlignment::Face);
    session_.setFaceExtrapolation(static_cast<editor::FaceExtrapolation>(extrapolation));
    session_.setOverlayPalette(overlayPalette(currentTokens()));
    session_.setPieSettings(loadPieSettings());
    initializeIcons();
    session_.setPieTrace([this](std::string_view event, std::string_view detail, std::chrono::milliseconds time) {
        // Once per refusal: pieRefusal() stays set until the next open, so an observer would repeat it.
        if (event == "refusal") statusBar()->showMessage(fromStd(detail), 4000);
        inputTraceEvent("pie", {{"event", fromStd(event)}, {"detail", fromStd(detail)},
                                {"session_ms", static_cast<qint64>(time.count())}});
    });
    buildMenus();
    buildStatusBar();
    buildTransformUi();
    buildClipboardUi();
    buildEntityUi();
    buildAreas();
    compilePanel_ = viewport_->compilePanel();
    compilePanel_->setGameContent(gameContent_.get());
    compilePanel_->setEditorSession(&session_);
    connect(compilePanel_, &CompilePanel::focusRequested, this, [this](DVec3 point) {
        AABB box;
        box.expand(point - DVec3(48.0));
        box.expand(point + DVec3(48.0));
        viewport_->centerOn(box);
    });
    connect(compilePanel_, &CompilePanel::frameRequested, this, [this] {
        if (const auto bounds = session_.selectionBounds()) viewport_->centerOn(*bounds);
    });
    viewport_->setEditorSession(&session_);
    viewport_->setOpenInstanceHandler([this](document::NodeId id) {
        return std::ranges::any_of(session_.instances().contents(), [id](const auto& item) { return item.instance == id && item.depth == 1; });
    }, [this](document::NodeId id) { openSelectedInstance(id); });
    viewport_->setGameContent(gameContent_.get());
    viewport_->setMaterialBrowser([this](QString current, QString what, std::function<void(QString)> choose) {
        openMaterialPicker(std::move(current), std::move(what), std::move(choose));
    });
    viewport_->setAssetBrowser([this](QString kind, QString current, QString what, std::function<void(QString)> choose) {
        openAssetPicker(std::move(kind), std::move(current), std::move(what), std::move(choose));
    });
    connect(viewport_, &ViewportArea::activeMaterialRequested, this, &MainWindow::activateMaterial);
    connect(viewport_, &ViewportArea::modelPlacementRequested, this, [this](const QString& path) {
        session_.setPlacementModel(path.toStdString());
        session_.perform(editor::EditorSession::Tool::Entity);
        statusBar()->showMessage(tr("Placing %1").arg(path), 3000);
    });
    connect(viewport_, &ViewportArea::prefabPlacementRequested, this, [this](const QString& file) {
        auto item = editor::prefabItem(std::filesystem::path(file.toStdString()));
        if (!item.bounds) {
            statusBar()->showMessage(tr("Cannot read the prefab %1").arg(file), 4000);
            return;
        }
        session_.setPlacementItem(std::move(item));
        session_.perform(editor::EditorSession::Tool::Entity);
        statusBar()->showMessage(tr("Placing prefab %1").arg(QFileInfo(file).completeBaseName()), 3000);
    });
    connect(viewport_, &ViewportArea::instanceCreateRequested, this, &MainWindow::createInstanceFromSelection);
    connect(viewport_, &ViewportArea::prefabCreateRequested, this, [this] { performEditAction("edit.create_prefab"); });
    connect(viewport_, &ViewportArea::modelPlacementFailed, this, [this](const QString& message) {
        statusBar()->showMessage(message, 4000);
    });
    connect(viewport_, &ViewportArea::materialStatusMessage, this, [this](const QString& message) {
        statusBar()->showMessage(message, 4000);
    });
    session_.confirmNonConvexMerge = QSettings().value("editing/confirmNonConvexMerge", true).toBool();
    session_.setPivotAtObjectOrigin(QSettings().value("editing/pivotAtObjectOrigin", true).toBool());
    session_.setModelEdgeOverlay(QSettings().value("view/modelEdgeOverlay", true).toBool());
    session_.setEntityIconsNearest(QSettings().value("view/entityIconsNearest", false).toBool());
    session_.hollowThickness = [this]() -> std::optional<double> {
        bool ok = false;
        const auto value = QInputDialog::getDouble(this, tr("Make Hollow"),
            tr("Wall thickness (negative grows outward):"), 16, -65536, 65536, 3, &ok);
        return ok ? std::optional<double>{value} : std::nullopt;
    };
    session_.confirmMerge = [this](const editor::MergePlan& plan) {
        QMessageBox box(QMessageBox::Warning, tr("Merge solids"),
            tr("Merging fills %1 units³ outside the selection. Merge anyway?").arg(plan.addedVolume, 0, 'g', 12),
            QMessageBox::Cancel, this);
        box.setObjectName(QStringLiteral("confirmNonConvexMerge"));
        auto* merge = box.addButton(tr("Merge"), QMessageBox::AcceptRole);
        box.setDefaultButton(QMessageBox::Cancel);
        auto* remember = new QCheckBox(tr("Don't ask again"), &box);
        box.setCheckBox(remember);
        box.exec();
        if (remember->isChecked()) {
            QSettings().setValue("editing/confirmNonConvexMerge", false);
            session_.confirmNonConvexMerge = false;
        }
        return box.clickedButton() == merge;
    };
    sessionObserver_ = session_.observe([this] {
        if (session_.pie()) {
            if (!piePopup_ || !piePopup_->isVisible()) {
                if (auto* pane = TransformInput::widgetFor(*session_.pieSource())) {
                    if (!piePopup_) piePopup_ = new PieMenuPopup(session_, this);
                    piePopup_->open(pane);
                }
            }
        } else if (piePopup_) piePopup_->sync();
        if (lastActiveMaterial_ != session_.newSolidStyle().material) {
            lastActiveMaterial_ = session_.newSolidStyle().material;
            QSettings().setValue(QStringLiteral("editor/newSolidMaterial"), fromStd(lastActiveMaterial_));
            updateActiveMaterialIndicator();
        }
        if (lastFaceExtrapolation_ != session_.faceExtrapolation()) {
            lastFaceExtrapolation_ = session_.faceExtrapolation();
            QSettings().setValue(QStringLiteral("editor/faceExtrapolation"), static_cast<int>(lastFaceExtrapolation_));
            const auto name = lastFaceExtrapolation_ == editor::FaceExtrapolation::Wrap ? tr("Wrap")
                            : lastFaceExtrapolation_ == editor::FaceExtrapolation::Projected ? tr("Projected") : tr("Match");
            statusBar()->showMessage(tr("Face extrapolation: %1").arg(name), 4000);
        }
        if (lastActiveTool_ != session_.activeTool()) {
            lastActiveTool_ = session_.activeTool();
            // Queued: the tool key may come from a pane that showRole replaces.
            if (lastActiveTool_ == editor::EditorSession::Tool::Apply)
                QMetaObject::invokeMethod(this, [this] {
                    if (session_.activeTool() == editor::EditorSession::Tool::Apply)
                        viewport_->showRole(viewport::AreaRole::FaceProperties, facePropertiesInWindow());
                }, Qt::QueuedConnection);
        }
        updateEditorActions();
        updateGridStatus();
        updateTitle();
        refreshEntityUi();
        modeLabel_->setText(modeSummary(session_));
        viewport_->updateModeBadges();
        syncInstanceWatches();
    });
    instanceWatcher_ = new QFileSystemWatcher(this);
    instanceWatcher_->setObjectName(QStringLiteral("instanceWatcher"));
    instanceReloadTimer_ = new QTimer(this);
    instanceReloadTimer_->setSingleShot(true);
    instanceReloadTimer_->setInterval(150);
    connect(instanceWatcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        pendingInstanceReloads_.insert(std::filesystem::path(path.toStdU16String()));
        instanceReloadTimer_->start();
    });
    connect(instanceWatcher_, &QFileSystemWatcher::directoryChanged, this, [this](const QString& changed) {
        // The notification only says something in the folder changed: reload the templates whose
        // file actually did, and pick up a missing template that came back.
        auto reload = instanceWatch_.directoryChanged(std::filesystem::path(changed.toStdU16String()));
        // A folder a missing template was waiting for may have appeared; listen deeper and look again.
        for (const auto& added : syncInstanceWatches()) reload.merge(instanceWatch_.directoryChanged(added));
        pendingInstanceReloads_.merge(reload);
        if (!pendingInstanceReloads_.empty()) instanceReloadTimer_->start();
    });
    connect(instanceReloadTimer_, &QTimer::timeout, this, &MainWindow::reloadChangedInstances);
    lastActiveTool_ = session_.activeTool();
    lastFaceExtrapolation_ = session_.faceExtrapolation();
    const auto presetName = QSettings().value(QStringLiteral("editor/preset"), QStringLiteral("Hammer")).toString();
    const bool touching = QSettings().value(QStringLiteral("editor/marqueeTouching"), false).toBool();
    session_.setMarqueeMode(touching ? editor::MarqueeMode::Touching : editor::MarqueeMode::Enclosed);
    const auto preset = presetStore_.find(presetName.toStdString());
    if (!preset) log::warn("keybinding preset '{}' unavailable: {}", presetName.toStdString(), preset.error().what());
    applyPreset(preset ? *preset : editor::hammerPreset());
    updateTitle();
    restoreLayout();
    autosaveTimer_ = new QTimer(this);
    autosaveTimer_->setObjectName(QStringLiteral("autosaveTimer"));
    connect(autosaveTimer_, &QTimer::timeout, this, &MainWindow::snapshotAutosave);
    if (QSettings().value("autosave/enabled", true).toBool())
        autosaveTimer_->start(std::clamp(QSettings().value("autosave/intervalMinutes", 5).toInt(), 1, 1440) * 60000);
    if (!qEnvironmentVariableIsSet("APEX_SKIP_RECOVERY"))
        QTimer::singleShot(0, this, &MainWindow::offerRecovery);
    QTimer::singleShot(0, this, [this] { inputTraceWindowStart(*this); });
}

MainWindow::~MainWindow() {
    compilePanel_->setDocument(nullptr);
    compilePanel_->setGameContent(nullptr);
    compilePanel_->setEditorSession(nullptr);
    delete piePopup_.data();
    closeMaterialPicker();
    session_.unobserve(sessionObserver_);
    viewport_->setEditorSession(nullptr);  // Also its panels and floating windows.
    session_.detach();
    // The area and its panels hold raw pointers to the document.
    if (viewport_) viewport_->setDocument(nullptr);
    if (gameContent_) gameContent_->clear();
}

OutlinerPanel* MainWindow::outliner() const { return viewport_->findChild<OutlinerPanel*>(); }

LogPanel* MainWindow::logPanel() const { return viewport_->findChild<LogPanel*>(); }

bool MainWindow::inspectorInWindow() { return QSettings().value(QStringLiteral("ui/inspectorWindow"), false).toBool(); }

void MainWindow::setInspectorInWindow(bool window) { QSettings().setValue(QStringLiteral("ui/inspectorWindow"), window); }

bool MainWindow::materialBrowserInWindow() {
    return QSettings().value(QStringLiteral("ui/materialBrowserWindow"), false).toBool();
}

void MainWindow::setMaterialBrowserInWindow(bool window) {
    QSettings().setValue(QStringLiteral("ui/materialBrowserWindow"), window);
}

bool MainWindow::facePropertiesInWindow() {
    return QSettings().value(QStringLiteral("ui/facePropertiesWindow"), false).toBool();
}

void MainWindow::setFacePropertiesInWindow(bool window) {
    QSettings().setValue(QStringLiteral("ui/facePropertiesWindow"), window);
}

bool MainWindow::compilationInWindow() {
    return QSettings().value(QStringLiteral("ui/compilationWindow"), false).toBool();
}

void MainWindow::setCompilationInWindow(bool window) {
    QSettings().setValue(QStringLiteral("ui/compilationWindow"), window);
}

void MainWindow::showCompilation() { viewport_->showRole(viewport::AreaRole::Compile, compilationInWindow()); }

void MainWindow::showInspector() { viewport_->showRole(viewport::AreaRole::Inspector, inspectorInWindow()); }

void MainWindow::buildMenus() {
    auto* file = menuBar()->addMenu(tr("&File"));
    auto* create = file->addAction(tr("&New"), QKeySequence::New, this, &MainWindow::newFile);
    create->setObjectName(QStringLiteral("actionNew"));
    auto* open = file->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::openDialog);
    open->setObjectName(QStringLiteral("actionOpen"));
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    recentMenu_->setObjectName(QStringLiteral("menuRecent"));
    file->addSeparator();
    saveAction_ = file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::save);
    saveAction_->setObjectName(QStringLiteral("actionSave"));
    saveAsAction_ = file->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, &MainWindow::saveAs);
    saveAsAction_->setObjectName(QStringLiteral("actionSaveAs"));
    saveAction_->setEnabled(false);
    saveAsAction_->setEnabled(false);
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto* marker = new QAction(tr("Mark input trace: felt wrong"), this);
    marker->setObjectName(QStringLiteral("actionInputTraceMarker"));
    marker->setShortcut(QKeySequence(Qt::Key_F9));
    marker->setShortcutContext(Qt::ApplicationShortcut);
    addAction(marker);
    connect(marker, &QAction::triggered, this, [this] {
        const int n = inputTraceMarker();
        statusBar()->showMessage(n ? tr("Input trace marker %1: felt wrong").arg(n)
                                   : tr("Input trace is disabled (set APEX_INPUT_LOG)"), 5000);
    });
    rebuildRecentMenu();

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    edit->setObjectName(QStringLiteral("menuEdit"));
    auto* selectionBar = addToolBar(tr("Selection"));
    selectionBar->setObjectName(QStringLiteral("toolbarSelection"));
    auto* modes = new QActionGroup(this);
    selectionBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    selectionBar->setIconSize(QSize(24, 24));
    for (const auto& entry : editor::actionRegistry().entries()) {
        if (entry.id != "edit.undo" && entry.id != "edit.redo" &&
            (!entry.id.starts_with("selection.") || entry.id == "selection.box")) continue;
        auto* action = edit->addAction(fromStd(entry.name));
        action->setObjectName(fromStd(entry.id));
        action->setToolTip(fromStd(entry.description));
        editorActions_.emplace(entry.id, action);
        if (entry.id.starts_with("selection.mode.")) {
            action->setCheckable(true);
            modes->addAction(action);
            selectionBar->addAction(action);
        } else if (entry.id == "selection.ignore_groups" || entry.id == "selection.marquee_toggle") {
            action->setCheckable(true);
            if (entry.id == "selection.ignore_groups") selectionBar->addSeparator();
            selectionBar->addAction(action);
        }
        connect(action, &QAction::triggered, this, [this, id = entry.id] {
            session_.perform(id);
            updateEditorActions();
        });
    }
    refreshSelectionIcons();
    auto* settings = menuBar()->addMenu(tr("&Settings"));
    auto* preferences = settings->addAction(tr("&Preferences..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                                            &MainWindow::openPreferences);
    preferences->setObjectName(QStringLiteral("actionPreferences"));
    settings->addSeparator();
    auto* autosaveSettings = settings->addAction(tr("Autosave..."), this, &MainWindow::configureAutosave);
    autosaveSettings->setObjectName(QStringLiteral("actionAutosaveSettings"));
    auto* undoMemory = settings->addAction(tr("Undo Memory Limit..."), this, [this] {
        bool accepted = false;
        const int limit = QInputDialog::getInt(this, tr("Undo Memory Limit"), tr("Memory limit (MiB):"),
                                               undoCapMiB(), 16, 8192, 16, &accepted);
        if (accepted) setUndoCapMiB(limit);
    });
    undoMemory->setObjectName(QStringLiteral("actionUndoMemoryLimit"));
    presetMenu_ = settings->addMenu(tr("Binding preset"));
    refreshPresetMenu();

    viewMenu_ = menuBar()->addMenu(tr("&View"));
    viewMenu_->setObjectName(QStringLiteral("menuView"));
    auto* grid = viewMenu_->addMenu(tr("Grid"));
    grid->setObjectName(QStringLiteral("menuGrid"));
    for (const auto& entry : editor::actionRegistry().entries()) {
        if (entry.category != "Grid") continue;
        auto* action = grid->addAction(fromStd(entry.name));
        action->setObjectName(fromStd(entry.id));
        action->setToolTip(fromStd(entry.description));
        editorActions_.emplace(entry.id, action);
        connect(action, &QAction::triggered, this, [this, id = entry.id] { session_.perform(id); });
    }

    auto* compileMenu = new QMenu(tr("&Compile"), this);
    menuBar()->insertMenu(viewMenu_->menuAction(), compileMenu);
    compileMenu->setObjectName(QStringLiteral("menuCompile"));
    auto* showCompile = compileMenu->addAction(tr("Show compilation"), this, &MainWindow::showCompilation);
    showCompile->setObjectName(QStringLiteral("compile.show"));
    compileMenu->addAction(tr("Compile"), this, [this] {
        showCompilation();
        compilePanel_->run();
    });
    for (const char* id : {"compile.compile_run", "compile.run_game"}) {
        const auto* entry = editor::actionRegistry().find(id);
        auto* action = compileMenu->addAction(fromStd(entry->name));
        action->setObjectName(fromStd(entry->id));
        action->setToolTip(fromStd(entry->description));
        editorActions_.emplace(entry->id, action);
        connect(action, &QAction::triggered, this, [this, id = entry->id] { performEditAction(id); });
    }
    compileMenu->addAction(tr("Cancel"), this, [this] { compilePanel_->cancel(); });

    addonsMenu_ = menuBar()->addMenu(tr("&Addons"));
    addonsMenu_->setObjectName(QStringLiteral("menuAddons"));
    const auto* reload = editor::actionRegistry().find("assets.reload");
    auto* reloadAction = new QAction(fromStd(reload->name), this);
    reloadAction->setObjectName(fromStd(reload->id));
    reloadAction->setToolTip(fromStd(reload->description));
    editorActions_.emplace(reload->id, reloadAction);
    connect(reloadAction, &QAction::triggered, this, &MainWindow::reloadGameContent);
    connect(addonsMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildAddonsMenu);
    rebuildAddonsMenu();

    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Apex"), this, [this] {
        const auto& v = version();
        QMessageBox::about(this, tr("About Apex"),
                           tr("Apex %1\nA Source engine map editor.\nQt %2 (LGPL-3.0).")
                               .arg(fromStd(v.string), QString::fromLatin1(qVersion())));
    });

    // Game profile picker lives in a toolbar so it is always visible.
    auto* bar = addToolBar(tr("Game"));
    bar->setObjectName(QStringLiteral("toolbarGame"));
    bar->setMovable(false);
    bar->addWidget(new QLabel(tr("Game:")));
    profileCombo_ = new QComboBox;
    profileCombo_->setObjectName(QStringLiteral("profilePicker"));
    profileCombo_->setMinimumWidth(220);
    profileCombo_->addItem(tr("(no game profiles)"));
    profileCombo_->setEnabled(false);
    bar->addWidget(profileCombo_);
    connect(profileCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i < 0 || static_cast<std::size_t>(i) >= profiles_.size()) return;
        QSettings().setValue(QStringLiteral("profile/last"), QString::fromStdString(profiles_[static_cast<std::size_t>(i)].name));
        loadEntityClasses();
        const auto* profile = currentProfile();
        session_.setConnectionContext(std::filesystem::path(path_.toStdU16String()),
            profile ? std::optional(profile->instanceSearchRoot()) : std::nullopt);
        log::info("game profile: {}", profiles_[static_cast<std::size_t>(i)].name);
        reloadGameContent();
    });
}

void MainWindow::reloadGameContent() {
    gameContent_->reload(currentProfile());
    if (viewport_) viewport_->setDocument(document_.get(), currentProfile());
}

void MainWindow::rebuildAddonsMenu() {
    auto* reload = editorActions_.at("assets.reload");
    for (auto* action : addonsMenu_->actions()) {
        addonsMenu_->removeAction(action);
        if (action == reload) continue;
        // Deferred: a toggle rebuilds the menu from inside its submenu's own event handling.
        if (auto* submenu = action->menu()) {
            submenu->setObjectName({});
            submenu->deleteLater();
        }
        else action->deleteLater();
    }
    const auto header = [this](const QString& text, const QString& path = {}) {
        auto* action = addonsMenu_->addAction(text);
        action->setEnabled(false);
        if (!path.isEmpty()) {
            action->setToolTip(path);
            action->setStatusTip(path);
        }
    };
    const auto* profile = profileCombo_ ? currentProfile() : nullptr;
    if (!profile) header(tr("No game profile"));
    else if (auto list = assets::readAddonList(*profile)) {
        const bool apex = list->source == assets::AddonSettings::List::Apex;
        if (apex) header(tr("Game profile list"));
        else if (list->file.empty()) header(tr("No addon list found"));
        else header(list->source == assets::AddonSettings::List::Hammer ? tr("Hammer list") : tr("Game list"),
                    fromStd(list->file.string()));
        const auto snapshot = std::make_shared<assets::AddonList>(std::move(*list));
        std::size_t workshopCount = 0, workshopMounted = 0, localCount = 0, localMounted = 0;
        for (const auto& entry : snapshot->entries) {
            if (!apex && entry.type == "steam") {
                ++workshopCount;
                workshopMounted += entry.mounted;
            } else {
                ++localCount;
                localMounted += apex || entry.mounted;
            }
        }
        const auto makeSubmenu = [this](const QString& title, const char* name) {
            auto* menu = addonsMenu_->addMenu(title);
            menu->setObjectName(QString::fromLatin1(name));
            return menu;
        };
        QMenu* workshop = workshopCount ? makeSubmenu(tr("Workshop (%1 of %2 on)")
            .arg(workshopMounted).arg(workshopCount), "menuAddonsWorkshop") : nullptr;
        QMenu* local = localCount ? makeSubmenu(tr("Local (%1 of %2 on)")
            .arg(localMounted).arg(localCount), "menuAddonsLocal") : nullptr;
        for (std::size_t i = 0; i < snapshot->entries.size(); ++i) {
            const auto& entry = snapshot->entries[i];
            auto* parent = !apex && entry.type == "steam" ? workshop : local;
            auto text = fromStd(entry.name);
            if (entry.resolved.empty()) text += tr(" (missing)");
            auto* action = parent->addAction(text);
            action->setCheckable(true);
            action->setChecked(apex || entry.mounted);
            action->setEnabled(!apex && !entry.resolved.empty());
            const auto tip = apex ? tr("Listed in the game profile's addons.mounted") : fromStd(entry.dir);
            action->setToolTip(tip);
            action->setStatusTip(tip);
            connect(action, &QAction::triggered, this, [this, list = snapshot, i](bool mounted) {
                if (auto result = assets::setAddonMounted(*list, i, mounted)) reloadGameContent();
                else {
                    statusBar()->showMessage(fromStd(result.error().what()), 5000);
                    log::warn("addons: {}", result.error().what());
                }
                rebuildAddonsMenu();
            });
        }
    } else header(fromStd(list.error().what()));
    addonsMenu_->addSeparator();
    addonsMenu_->addAction(reload);
}

void MainWindow::applyPreset(const editor::Preset& preset) {
    session_.setPreset(preset);
    // Native pane key delivery uses this same session keymap.
    for (const auto& [id, action] : editorActions_) {
        QList<QKeySequence> sequences;
        const auto& binding = session_.preset().bindings.at(id);
        const bool transformAction = isPaneKeyAction(id);
        const bool toolScoped = !binding.context.toolId.empty();
        if (!toolScoped) if (const auto* chords = std::get_if<std::vector<editor::KeyChord>>(&binding.value)) {
            for (const auto& chord : *chords) {
                // A bare modifier is a held key (snap.suspend style), never a shortcut.
                if (isTransformAction(id) && (chord.key == "Ctrl" || chord.key == "Alt" || chord.key == "Shift")) continue;
                sequences.push_back(keySequence(chord));
            }
        }
        action->setShortcuts(sequences);
        if (id.starts_with("selection.mode.") || id == "selection.ignore_groups" || id == "selection.marquee_toggle") {
            const auto* entry = editor::actionRegistry().find(id);
            const auto shortcuts = shortcutText(session_.preset(), id);
            action->setToolTip(fromStd(entry->name) +
                (shortcuts.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(shortcuts)) +
                QStringLiteral(": ") + fromStd(entry->description));
        }
        // Unmodified keys (W/E/R, arrows, Shift+L) belong to the focused pane, which routes them
        // through the same keymap; a window shortcut would steal them from the outliner.
        if (transformAction || toolScoped) {
            bool plain = true;
            if (const auto* chords = std::get_if<std::vector<editor::KeyChord>>(&binding.value)) {
                for (const auto& chord : *chords) plain = plain && !chord.ctrl && !chord.alt;
            }
            action->setShortcutContext(plain ? Qt::WidgetShortcut : Qt::WindowShortcut);
        }
    }
    if (presetMenu_) {
        for (auto* action : presetMenu_->actions())
            action->setChecked(action->data().toString() == fromStd(preset.name));
    }
    QSettings().setValue(QStringLiteral("editor/preset"), fromStd(preset.name));
}

void MainWindow::refreshPresetMenu() {
    presetMenu_->clear();
    auto* group = new QActionGroup(presetMenu_);
    std::vector<std::string> names;
    for (const auto name : editor::presetNames()) names.emplace_back(name);
    const auto saved = presetStore_.list();
    const auto addPreset = [&](const std::string& name) {
        auto* action = presetMenu_->addAction(fromStd(name));
        action->setObjectName(QStringLiteral("preset.") + fromStd(name));
        action->setData(fromStd(name));
        action->setCheckable(true);
        action->setChecked(name == session_.preset().name);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, name] {
            if (const auto preset = presetStore_.find(name)) applyPreset(*preset);
            else log::warn("keybinding preset '{}': {}", name, preset.error().what());
        });
    };
    for (const auto& name : names) addPreset(name);
    if (!saved.empty()) presetMenu_->addSeparator();
    for (const auto& name : saved) addPreset(name);
}

void MainWindow::setAutosave(bool enabled, int intervalMinutes) {
    QSettings settings;
    settings.setValue("autosave/enabled", enabled);
    settings.setValue("autosave/intervalMinutes", intervalMinutes);
    autosaveTimer_->stop();
    if (enabled) autosaveTimer_->start(intervalMinutes * 60000);
}

void MainWindow::setUndoCapMiB(int mib) {
    QSettings().setValue(QStringLiteral("history/undoCapMiB"), mib);
    if (document_) document_->setUndoCap(static_cast<std::size_t>(mib) * 1024 * 1024);
    for (auto* history : findChildren<HistoryPanel*>()) history->refresh();
    updateEditorActions();
}

void MainWindow::activateMaterial(const QString& name) {
    setNewSolidStyle(name.toStdString(), session_.newSolidStyle().alignment);
    if (session_.activeTool() == editor::EditorSession::Tool::Apply &&
        session_.selection().mode() == editor::SelectionMode::Faces && !session_.selection().ordered().empty()) {
        std::vector<document::SideId> sides;
        for (const auto target : session_.selection().ordered())
            if (const auto* side = std::get_if<document::SideId>(&target)) sides.push_back(*side);
        const auto changed = session_.applyMaterial(sides, name.toStdString());
        statusBar()->showMessage(changed
            ? tr("Applied %1 to %2 selected face(s); active material updated").arg(name).arg(changed)
            : tr("Active material updated; selected faces already use %1").arg(name), 5000);
        return;
    }
    statusBar()->showMessage(tr("Active material: %1").arg(name), 4000);
}
void MainWindow::closeMaterialPicker() {
    QObject::disconnect(areaPickChosen_);
    QObject::disconnect(areaPickCancelled_);
    if (areaMaterialPicker_) areaMaterialPicker_->setPickMode(false);
    areaMaterialPicker_.clear();
    if (areaAssetPicker_) areaAssetPicker_->setPickMode(false);
    areaAssetPicker_.clear();
    delete materialPicker_.data();
}
void MainWindow::openMaterialPicker(QString current, QString what, std::function<void(QString)> choose) {
    closeMaterialPicker();
    if (!materialBrowserInWindow() &&
        !viewport_->viewportLayout().paneIds(viewport::AreaRole::Materials).empty()) {
        auto* panel = qobject_cast<MaterialsPanel*>(viewport_->showRole(viewport::AreaRole::Materials, false));
        if (panel) {
            areaMaterialPicker_ = panel;
            panel->setPickMode(true, current, what);
            areaPickChosen_ = connect(panel, &MaterialsPanel::materialChosen, this,
                [this, panel, choose = std::move(choose)](const QString& value) mutable {
                    QObject::disconnect(areaPickChosen_);
                    QObject::disconnect(areaPickCancelled_);
                    panel->setPickMode(false);
                    areaMaterialPicker_.clear();
                    choose(value);
                });
            areaPickCancelled_ = connect(panel, &MaterialsPanel::pickCancelled, this, &MainWindow::closeMaterialPicker);
            return;
        }
    }
    auto* dialog = new QDialog(this, Qt::Tool);
    materialPicker_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("materialPicker"));
    dialog->setWindowTitle(tr("Choose Material"));
    auto* layout = new QVBoxLayout(dialog);
    auto* panel = new MaterialsPanel(dialog);
    panel->setGameContent(gameContent_.get());
    panel->setDocument(document_.get());
    panel->setEditorSession(&session_);
    panel->setPickMode(true, current, what);
    connect(panel, &MaterialsPanel::materialChosen, dialog, [dialog, choose = std::move(choose)](const QString& value) {
        choose(value);
        dialog->close();
    });
    // The panel takes Esc in pick mode, so the dialog never sees it.
    connect(panel, &MaterialsPanel::pickCancelled, dialog, &QDialog::close);
    layout->addWidget(panel);
    dialog->resize(720, 480);
    dialog->show();
}
void MainWindow::openAssetPicker(QString kind, QString current, QString what, std::function<void(QString)> choose) {
    using viewport::AreaRole;
    AreaRole role;
    QString title;
    if (kind == "model") { role = AreaRole::Models; title = tr("Choose Model"); }
    else if (kind == "sound") { role = AreaRole::Sounds; title = tr("Choose Sound"); }
    else if (kind == "particle") { role = AreaRole::Particles; title = tr("Choose Particle System"); }
    else return;
    closeMaterialPicker();
    if (!materialBrowserInWindow() && !viewport_->viewportLayout().paneIds(role).empty()) {
        auto* panel = qobject_cast<AssetBrowserPanel*>(viewport_->showRole(role, false));
        if (panel) {
            areaAssetPicker_ = panel;
            panel->setPickMode(true, current, what);
            areaPickChosen_ = connect(panel, &AssetBrowserPanel::assetChosen, this,
                [this, choose = std::move(choose)](const QString& value) {
                    closeMaterialPicker();
                    choose(value);
                });
            areaPickCancelled_ = connect(panel, &AssetBrowserPanel::pickCancelled, this, &MainWindow::closeMaterialPicker);
            return;
        }
    }
    auto* dialog = new QDialog(this, Qt::Tool);
    materialPicker_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("assetPicker"));
    dialog->setWindowTitle(title);
    auto* layout = new QVBoxLayout(dialog);
    auto* panel = qobject_cast<AssetBrowserPanel*>(createPanel(role, document_.get(), &session_, gameContent_.get()));
    layout->addWidget(panel);
    viewport_->setModelPicker(qobject_cast<ModelsPanel*>(panel));
    panel->setPickMode(true, current, what);
    connect(panel, &AssetBrowserPanel::assetChosen, dialog, [dialog, panel, choose = std::move(choose)](const QString& value) {
        // Disconnect before the callback: a second activation cannot apply the same pick twice.
        QObject::disconnect(panel, nullptr, dialog, nullptr);
        dialog->close();
        choose(value);
    });
    connect(panel, &AssetBrowserPanel::pickCancelled, dialog, &QDialog::close);
    dialog->resize(720, 480);
    dialog->show();
}
void MainWindow::setNewSolidStyle(std::string material, geometry::UvAlignment alignment) {
    session_.setNewSolidMaterial(std::move(material));
    session_.setNewSolidUvAlignment(alignment);
    QSettings settings;
    settings.setValue(QStringLiteral("editor/newSolidMaterial"), fromStd(session_.newSolidStyle().material));
    settings.setValue(QStringLiteral("editor/newSolidWorldUv"),
                      session_.newSolidStyle().alignment == geometry::UvAlignment::World);
    updateActiveMaterialIndicator();
}

void MainWindow::updateActiveMaterialIndicator() {
    if (!activeMaterialLabel_) return;
    const auto& name = session_.newSolidStyle().material;
    activeMaterialLabel_->setText(fromStd(name));
    QString shader;
    QPixmap swatch(22, 22);
    swatch.fill(QColor(96, 99, 104));
    if (gameContent_) {
        if (const auto snapshot = gameContent_->snapshot())
            if (const auto* entry = snapshot->find(name)) shader = fromStd(entry->shader);
        if (auto* thumbnails = gameContent_->thumbnails())
            if (const auto thumb = thumbnails->request(name, 22))
                swatch = QPixmap::fromImage(QImage(reinterpret_cast<const uchar*>(thumb->rgba.data()),
                    thumb->width, thumb->height, thumb->width * 4, QImage::Format_RGBA8888).copy());
    }
    activeMaterialLabel_->setIcon(QIcon(swatch));
    auto tip = shader.isEmpty() ? fromStd(name) : fromStd(name) + QStringLiteral("\n") + shader;
    if (gameContent_) {
        const auto missing = gameContent_->missingAssetTooltip("materials/" + name + ".vmt");
        if (!missing.isEmpty()) tip += '\n' + missing;
    }
    activeMaterialLabel_->setToolTip(tip);
}

void MainWindow::openPreferences() {
    auto* dialog = new SettingsDialog(*this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::updateEditorActions() {
    for (const auto& [id, action] : editorActions_) {
        action->setEnabled(session_.document() != nullptr);
        if (id == "instance.create") action->setEnabled(session_.document() && !editor::selectedNodes(session_.selection()).empty());
        if (id == "instance.collapse_all")
            action->setEnabled(session_.document() && (!session_.instances().contents().empty() || !session_.instances().problems().empty()));
        if (id == "instance.open" || id == "instance.collapse") {
            bool selectedInstance = false;
            for (const auto node : editor::selectedNodes(session_.selection())) {
                const auto* found = session_.document() ? session_.document()->get(*session_.document()->handle(node)) : nullptr;
                const auto* classname = found ? found->block->find("classname") : nullptr;
                if (classname && classname->decodedValue().value_or("") == "func_instance") selectedInstance = true;
            }
            if (id == "instance.collapse") action->setEnabled(selectedInstance);
            else {
                const auto selected = editor::selectedNodes(session_.selection());
                const bool resolved = selected.size() == 1 && std::ranges::any_of(session_.instances().contents(), [&](const auto& item) {
                    return item.instance == selected.front() && item.depth == 1;
                });
                action->setEnabled(resolved);
                action->setToolTip(resolved ? tr("Open this instance in another Apex window.")
                    : tr("The selected instance file could not be resolved."));
            }
        }
        if (id == "assets.reload" || id.starts_with("compile.") || id == "view.properties" || id.starts_with("workspace.")) action->setEnabled(true);  // Window actions.
        if (std::ranges::find(kToolActions, std::string_view(id)) != std::end(kToolActions)) {
            action->setEnabled(session_.isToolEnabled(id));
            action->setChecked(session_.activeTool() == id);
            if (toolButton_ && action->isChecked()) toolButton_->setText(action->text());
        }
        if (id.starts_with("edit.") && !id.starts_with("edit.flip.")) {
            const bool have = session_.document() != nullptr;
            const bool objects = have && !editor::selectedNodes(session_.selection()).empty();
            if (id == "edit.copy" || id == "edit.cut" || id == "edit.delete" || id == "edit.duplicate" ||
                id == "edit.group" || id == "edit.ungroup" || id == "edit.tie_to_entity" || id == "edit.move_to_world" ||
                id == "edit.carve" || id == "edit.hollow" || id == "edit.merge" || id == "edit.create_prefab")
                action->setEnabled(objects);
            if (id == "edit.paste" || id == "edit.paste_in_place" || id == "edit.paste_special")
                action->setEnabled(have && clipboardHasFragment_);
        }
        if (id == "view.hide" || id == "view.hide_unselected" || id == "view.isolate") {
            const bool objects = session_.document() && !editor::selectedNodes(session_.selection()).empty();
            action->setEnabled(objects || (id == "view.isolate" && session_.viewState().isolate.has_value()));
        }
        if (id == "view.isolate") action->setChecked(session_.viewState().isolate.has_value());
        if (id == "view.center_selection") action->setEnabled(session_.document() && !session_.selection().ordered().empty());
        if (id == "edit.undo") action->setEnabled(document_ && document_->undoCount() != 0);
        if (id == "edit.redo") action->setEnabled(document_ && document_->redoCount() != 0);
        if (id == "selection.ignore_groups") action->setChecked(session_.selection().ignoreGroups());
        if (id == "selection.marquee_toggle") {
            const bool touching = session_.marqueeMode() == editor::MarqueeMode::Touching;
            action->setChecked(touching);
            action->setText(touching ? tr("Marquee: Touching") : tr("Marquee: Enclosed"));
            if (savedMarqueeTouching_ != touching) {
                QSettings().setValue(QStringLiteral("editor/marqueeTouching"), touching);
                savedMarqueeTouching_ = touching;
            }
        }
        if (id == "gizmo.mode.translate") action->setChecked(session_.gizmoMode() == editor::GizmoMode::Translate);
        if (id == "gizmo.mode.rotate") action->setChecked(session_.gizmoMode() == editor::GizmoMode::Rotate);
        if (id == "gizmo.mode.scale") action->setChecked(session_.gizmoMode() == editor::GizmoMode::Scale);
        if (id == "gizmo.space.toggle") {
            const bool local = session_.gizmoSpace() == editor::GizmoSpace::Local;
            action->setChecked(local);
            action->setText(local ? tr("Space: Local") : tr("Space: Global"));
        }
        if (id == "vertex.mode.vertex" || id == "vertex.mode.edge" || id == "vertex.mode.face") {
            const auto* vertex = dynamic_cast<const editor::VertexTool*>(&session_.activeToolObject());
            const auto mode = id == "vertex.mode.vertex" ? editor::ElementMode::Vertex
                            : id == "vertex.mode.edge" ? editor::ElementMode::Edge : editor::ElementMode::Face;
            action->setEnabled(vertex != nullptr);
            action->setChecked(vertex && vertex->elementMode() == mode);
        }
        if (id == "snap.mode.toggle") {
            const bool relative = session_.snapSettings().mode == editor::SnapMode::Relative;
            action->setChecked(relative);
            action->setText(relative ? tr("Snap: Relative") : tr("Snap: Absolute"));
        }
        if (id == "transform.texture_lock.toggle") action->setChecked(session_.textureLock());
        if (id == "gizmo.pivot.origin.toggle") action->setChecked(session_.pivotAtObjectOrigin());
        if (id == "view.model_overlay.toggle") action->setChecked(session_.modelEdgeOverlay());
        const std::pair<const char*, editor::SelectionMode> modes[] = {
            {"selection.mode.groups", editor::SelectionMode::Groups},
            {"selection.mode.objects", editor::SelectionMode::Objects},
            {"selection.mode.solids", editor::SelectionMode::Solids},
            {"selection.mode.faces", editor::SelectionMode::Faces}};
        for (const auto& [modeId, mode] : modes) {
            if (id == modeId) action->setChecked(session_.selection().mode() == mode);
        }
    }
    for (const auto& [tool, options] : toolOptions_)
        for (auto* option : options) option->setVisible(session_.activeTool() == tool);
}

void MainWindow::reportCollapse(const editor::InstanceCollapseReport& report) {
    std::size_t failed = 0;
    std::string firstReason;
    for (const auto& item : report) if (!item.collapsed) {
        if (!failed) firstReason = item.reason;
        ++failed;
        log::warn("instance collapse failed for {}: {}", item.instance, item.reason);
    }
    if (!failed) {
        statusBar()->showMessage(tr("Collapsed %1 instance(s)").arg(report.size()), 3000);
        return;
    }
    const auto collapsed = report.size() - failed;
    statusBar()->showMessage(tr("Collapsed %1, could not collapse %2: %3")
        .arg(collapsed).arg(failed).arg(fromStd(firstReason)), 15000);
}

void MainWindow::collapseSelectedInstances() {
    const auto selected = editor::selectedNodes(session_.selection());
    std::vector<document::NodeId> instances;
    for (const auto id : selected) {
        const auto handle = session_.document()->handle(id);
        const auto* entity = handle ? session_.document()->get(*handle) : nullptr;
        const auto* classname = entity ? entity->block->find("classname") : nullptr;
        if (classname && classname->decodedValue().value_or("") == "func_instance") instances.push_back(id);
    }
    if (auto result = session_.collapseInstances(instances)) reportCollapse(*result);
    else { log::warn("collapse instances: {}", result.error().what()); statusBar()->showMessage(fromStd(result.error().what()), 5000); }
}

void MainWindow::openSelectedInstance(document::NodeId instance) {
    if (!instance) {
        const auto selected = editor::selectedNodes(session_.selection());
        if (selected.size() != 1) return;
        instance = selected.front();
    }
    const auto found = std::ranges::find_if(session_.instances().contents(), [&](const auto& item) {
        return item.instance == instance && item.depth == 1;
    });
    if (found == session_.instances().contents().end()) return;
    const auto executable = QCoreApplication::applicationFilePath();
    const QStringList arguments{QString::fromStdU16String(found->path.u16string())};
    const bool started = instanceLauncher_ ? instanceLauncher_(executable, arguments)
        : QProcess::startDetached(executable, arguments);
    if (!started) statusBar()->showMessage(tr("Could not open instance: %1").arg(arguments.front()), 5000);
}

std::vector<std::filesystem::path> MainWindow::syncInstanceWatches() {
    std::vector<std::filesystem::path> added;
    if (!instanceWatcher_) return added;
    std::set<std::filesystem::path> dependencies;
    for (const auto& [path, users] : session_.instances().dependencies()) if (!users.empty()) dependencies.insert(path);
    const auto watches = instanceWatch_.sync(dependencies, session_.instances().missing());
    std::set<QString> files, directories;
    for (const auto& path : watches.files) files.insert(QDir::fromNativeSeparators(QString::fromStdU16String(path.u16string())));
    for (const auto& path : watches.directories) directories.insert(QDir::fromNativeSeparators(QString::fromStdU16String(path.u16string())));
    for (const auto& path : instanceWatcher_->files()) if (!files.contains(path)) instanceWatcher_->removePath(path);
    for (const auto& path : instanceWatcher_->directories()) if (!directories.contains(path)) instanceWatcher_->removePath(path);
    for (const auto& path : files) if (QFileInfo::exists(path) && !instanceWatcher_->files().contains(path)) instanceWatcher_->addPath(path);
    for (const auto& path : directories)
        if (QFileInfo(path).isDir() && !instanceWatcher_->directories().contains(path) && instanceWatcher_->addPath(path))
            added.emplace_back(path.toStdU16String());
    for (const auto& problem : session_.instances().problems()) {
        const auto key = problem.path.empty() ? std::filesystem::path(problem.file) : problem.path;
        const std::string signature = problem.message;
        if (reportedInstanceProblems_[key] != signature) {
            reportedInstanceProblems_[key] = signature;
            log::warn("{}", problem.message);
        }
    }
    return added;
}

void MainWindow::reloadChangedInstances() {
    const auto changed = std::exchange(pendingInstanceReloads_, {});
    for (const auto& path : changed) {
        const auto result = session_.invalidateInstance(path);
        instanceWatch_.restamp(path);
        if (result.problem) {
            const auto signature = result.problem->message;
            if (reportedInstanceProblems_[path] != signature) {
                reportedInstanceProblems_[path] = signature;
                log::warn("instance reload failed for {}: {}", path.string(), signature);
                statusBar()->showMessage(fromStd(signature), 6000);
            }
        } else {
            reportedInstanceProblems_.erase(path);
            log::info("reloaded instance template {}", path.string());
        }
    }
    syncInstanceWatches();
    if (viewport_) viewport_->update();
}

void MainWindow::buildAreas() {
    // The central widget is the workspace tabs over the area tree (0.2.12). Every panel is an
    // area role, so there are no docks: the outliner, inspector, visgroups, history and log are
    // areas the user can split, switch, collapse or open in their own window.
    auto* central = new QWidget;
    central->setObjectName(QStringLiteral("centralAreas"));
    auto* column = new QVBoxLayout(central);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    viewport_ = new ViewportArea;
    workspaces_ = new WorkspaceBar(*viewport_);
    column->addWidget(workspaces_);
    column->addWidget(viewport_, 1);
    setCentralWidget(central);

    auto* areas = viewMenu_->addMenu(tr("Show area"));
    areas->setObjectName(QStringLiteral("menuShowArea"));
    for (const auto role : viewport::kAreaRoles) {
        if (role == viewport::AreaRole::Viewport) continue;
        auto* action = areas->addAction(roleTitle(role), this, [this, role] { viewport_->showRole(role, false); });
        action->setObjectName(QStringLiteral("actionShowArea_") + QString::fromLatin1(viewport::nameOf(role)));
        action->setToolTip(tr("Expand this workspace's %1 area, or open it in a window if it has none").arg(roleTitle(role)));
    }
    for (const char* id : {"view.properties", "workspace.next", "workspace.previous"}) {
        const auto* entry = editor::actionRegistry().find(id);
        auto* action = viewMenu_->addAction(fromStd(entry->name));
        action->setObjectName(fromStd(entry->id));
        action->setToolTip(fromStd(entry->description));
        editorActions_.emplace(entry->id, action);
        connect(action, &QAction::triggered, this, [this, id = entry->id] {
            if (id == "view.properties") showInspector();
            else workspaces_->step(id == "workspace.next" ? 1 : -1);
        });
    }
    viewMenu_->addSeparator();
    themeMenu_ = viewMenu_->addMenu(tr("Theme"));
    themeMenu_->setObjectName(QStringLiteral("themeMenu"));
    refreshThemeMenu();
    viewMenu_->addSeparator();
    viewMenu_->addAction(tr("Reset cameras"), viewport_, &ViewportArea::resetCameras);
    viewMenu_->addAction(tr("Reset workspace"), workspaces_, &WorkspaceBar::resetActive)
        ->setObjectName(QStringLiteral("actionResetWorkspace"));

    connect(viewport_, &ViewportArea::cameraPositionChanged, this, [this](double x, double y, double z) {
        cameraLabel_->setText(tr("Cam: %1 %2 %3").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1).arg(z, 0, 'f', 1));
    });
    connect(viewport_, &ViewportArea::frameTimeChanged, this,
            [this](double ms) { frameLabel_->setText(tr("%1 ms").arg(ms, 0, 'f', 1)); });
    connect(viewport_, &ViewportArea::gpuNameChanged, this,
            [this](const QString& n) { gpuLabel_->setText(tr("GPU: %1").arg(n)); });
}

void MainWindow::updateGridStatus() {
    const auto& style = session_.newSolidStyle();
    const bool world = style.alignment == geometry::UvAlignment::World;
    uvAlignmentAction_->setChecked(world);
    uvAlignmentAction_->setText(world ? tr("UV: World") : tr("UV: Face"));
    const auto grid = tr("Grid: %1").arg(session_.gridSize(), 0, 'g', 12);
    const auto resolved = session_.resolvedWorkPlane();
    const auto plane = !resolved ? tr("Plane: ground")
        : resolved->source == editor::WorkPlaneSource::Pinned ? tr("Plane: pinned") : tr("Plane: face (hover)");
    if (gridLabel_->text() != grid) gridLabel_->setText(grid);
    if (planeLabel_->text() != plane) planeLabel_->setText(plane);
    const auto readout = QString::fromStdString(session_.activeToolObject().readout().status);
    if (readoutLabel_->text() != readout) readoutLabel_->setText(readout);
}

void MainWindow::buildStatusBar() {
    gridLabel_ = new QLabel;
    gridLabel_->setObjectName(QStringLiteral("statusGrid"));
    planeLabel_ = new QLabel;
    planeLabel_->setObjectName(QStringLiteral("statusPlane"));
    readoutLabel_ = new QLabel;
    readoutLabel_->setObjectName(QStringLiteral("statusReadout"));
    modeLabel_ = new QLabel;
    modeLabel_->setObjectName(QStringLiteral("statusMode"));
    modeLabel_->setToolTip(tr("Current editing mode"));
    statusBar()->addPermanentWidget(modeLabel_);
    statusBar()->addWidget(readoutLabel_);
    cameraLabel_ = new QLabel(tr("Cam: -"));
    frameLabel_ = new QLabel(tr("- ms"));
    gpuLabel_ = new QLabel(tr("GPU: -"));
    activeMaterialLabel_ = new QToolButton;
    activeMaterialLabel_->setObjectName(QStringLiteral("activeMaterialIndicator"));
    activeMaterialLabel_->setAutoRaise(true);
    activeMaterialLabel_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    activeMaterialLabel_->setIconSize(QSize(22, 22));
    connect(activeMaterialLabel_, &QToolButton::clicked, this, [this] {
        const auto current = fromStd(session_.newSolidStyle().material);
        // Choosing here only changes the active material; unlike a Materials panel click it never
        // applies to the selected faces.
        openMaterialPicker(current, tr("the active material"), [this](QString value) {
            setNewSolidStyle(value.toStdString(), session_.newSolidStyle().alignment);
            statusBar()->showMessage(tr("Active material: %1").arg(value), 4000);
        });
    });
    for (auto* l : {gridLabel_, planeLabel_, cameraLabel_, frameLabel_, gpuLabel_}) statusBar()->addPermanentWidget(l);
    statusBar()->addPermanentWidget(activeMaterialLabel_);
    uvAlignmentAction_ = new QAction(tr("UV: Face"), this);
    uvAlignmentAction_->setObjectName(QStringLiteral("newSolidWorldUv"));
    uvAlignmentAction_->setCheckable(true);
    uvAlignmentAction_->setToolTip(tr("Texture alignment for new solids"));
    connect(uvAlignmentAction_, &QAction::triggered, this, [this](bool world) {
        setNewSolidStyle(session_.newSolidStyle().material,
                         world ? geometry::UvAlignment::World : geometry::UvAlignment::Face);
    });
    auto* uvButton = new QToolButton;
    uvButton->setObjectName(QStringLiteral("statusUvAlignment"));
    uvButton->setDefaultAction(uvAlignmentAction_);
    uvButton->setAutoRaise(true);
    statusBar()->addPermanentWidget(uvButton);
    updateGridStatus();
    modeLabel_->setText(modeSummary(session_));
    connect(gameContent_.get(), &GameContent::contentChanged, this, &MainWindow::updateActiveMaterialIndicator);
    connect(gameContent_.get(), &GameContent::thumbnailsReady, this, &MainWindow::updateActiveMaterialIndicator);
    connect(gameContent_.get(), &GameContent::indexReady, this, &MainWindow::updateActiveMaterialIndicator);
    updateActiveMaterialIndicator();
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::buildTransformUi() {
    const auto make = [this](const char* id, QMenu* menu, bool checkable) {
        const auto* entry = editor::actionRegistry().find(id);
        auto* action = menu->addAction(fromStd(entry->name));
        action->setObjectName(fromStd(entry->id));
        action->setToolTip(fromStd(entry->description));
        action->setCheckable(checkable);
        editorActions_.emplace(entry->id, action);
        connect(action, &QAction::triggered, this, [this, id = entry->id] {
            if (id == "view.instances") {
                const auto current = session_.instanceVisibility();
                const auto next = current == document::InstanceVisibility::Hidden ? document::InstanceVisibility::Tinted
                    : current == document::InstanceVisibility::Tinted ? document::InstanceVisibility::Normal
                    : document::InstanceVisibility::Hidden;
                session_.setInstanceVisibility(next);
                viewport_->updateModeBadges();
            } else if (id == "transform.dialog") openTransformDialog();
            else if (id == "view.center_selection") centerViewsOnSelection();
            else session_.perform(id);
            // The toolbar toggles are the preferences; keep them across sessions.
            if (id == "gizmo.pivot.origin.toggle")
                QSettings().setValue("editing/pivotAtObjectOrigin", session_.pivotAtObjectOrigin());
            if (id == "view.model_overlay.toggle")
                QSettings().setValue("view/modelEdgeOverlay", session_.modelEdgeOverlay());
            updateEditorActions();
        });
        return action;
    };

    viewMenu_->addSeparator();
    for (const char* id : {"view.hide", "view.unhide", "view.isolate", "view.hide_unselected"})
        make(id, viewMenu_, std::string_view(id) == "view.isolate");
    viewMenu_->addSeparator();
    make("view.center_selection", viewMenu_, false);
    viewMenu_->addSeparator();
    auto* modelOverlay = make("view.model_overlay.toggle", viewMenu_, true);
    make("view.instances", viewMenu_, false);
    viewMenu_->addSeparator();

    auto* edit = findChild<QMenu*>(QStringLiteral("menuEdit"));
    auto* instancing = menuBar()->addMenu(tr("&Instancing"));
    instancing->setObjectName(QStringLiteral("menuInstancing"));
    for (const char* id : {"instance.create", "instance.open", "instance.collapse", "instance.collapse_all"}) {
        const auto* entry = editor::actionRegistry().find(id);
        auto* action = instancing->addAction(fromStd(entry->name));
        action->setObjectName(fromStd(entry->id));
        action->setToolTip(fromStd(entry->description));
        editorActions_.emplace(entry->id, action);
        connect(action, &QAction::triggered, this, [this, id = std::string_view(id)] {
            if (id == "instance.create") createInstanceFromSelection();
            else if (id == "instance.open") openSelectedInstance();
            else if (id == "instance.collapse") collapseSelectedInstances();
            else if (id == "instance.collapse_all") {
                if (auto result = session_.collapseAllInstances()) reportCollapse(*result);
                else { log::warn("collapse all instances: {}", result.error().what()); statusBar()->showMessage(fromStd(result.error().what()), 5000); }
            }
            updateEditorActions();
        });
    }
    auto* transform = edit->addMenu(tr("Transform"));
    transform->setObjectName(QStringLiteral("menuTransform"));
    make("transform.dialog", transform, false);
    transform->addSeparator();
    for (const char* id : {"edit.flip.x", "edit.flip.y", "edit.flip.z"}) make(id, transform, false);
    transform->addSeparator();
    for (const char* id : {"nudge.left", "nudge.right", "nudge.up", "nudge.down"}) make(id, transform, false);
    transform->addSeparator();
    auto* textureLock = make("transform.texture_lock.toggle", transform, true);

    auto* gizmo = viewMenu_->addMenu(tr("Gizmo"));
    gizmo->setObjectName(QStringLiteral("menuGizmo"));
    auto* modes = new QActionGroup(this);
    std::vector<QAction*> modeActions;
    for (const char* id : {"gizmo.mode.translate", "gizmo.mode.rotate", "gizmo.mode.scale"}) {
        auto* action = make(id, gizmo, true);
        modes->addAction(action);
        modeActions.push_back(action);
    }
    gizmo->addSeparator();
    auto* space = make("gizmo.space.toggle", gizmo, true);
    make("gizmo.pivot.cycle", gizmo, false);
    auto* pivotOrigin = make("gizmo.pivot.origin.toggle", gizmo, true);
    auto* snap = make("snap.mode.toggle", gizmo, true);

    auto* bar = addToolBar(tr("Transform"));
    bar->setObjectName(QStringLiteral("toolbarTransform"));
    for (auto* action : modeActions) bar->addAction(action);
    bar->addSeparator();
    bar->addAction(snap);
    bar->addAction(textureLock);
    bar->addAction(pivotOrigin);
    bar->addAction(modelOverlay);
    bar->addAction(space);

    // The same actions as checkable buttons in the status bar (always visible, even with the
    // toolbar hidden).
    const std::pair<const char*, QAction*> buttons[] = {
        {"statusSnapMode", snap}, {"statusTextureLock", textureLock}, {"statusPivotOrigin", pivotOrigin},
        {"statusModelOverlay", modelOverlay}};
    for (const auto& [name, action] : buttons) {
        auto* button = new QToolButton;
        button->setObjectName(QString::fromLatin1(name));
        button->setDefaultAction(action);
        button->setAutoRaise(true);
        statusBar()->addPermanentWidget(button);
    }
}

void MainWindow::buildEntityUi() {
    // Next to Selection: when the row overflows, the transform controls (also in the status bar) go
    // behind the extension button rather than the active tool's options.
    auto* bar = new QToolBar(tr("Tools"), this);
    insertToolBar(findChild<QToolBar*>(QStringLiteral("toolbarGame")), bar);
    bar->setObjectName(QStringLiteral("toolbarTools"));
    toolButton_ = new QToolButton(bar);
    toolButton_->setObjectName(QStringLiteral("toolDropDown"));
    toolButton_->setPopupMode(QToolButton::InstantPopup);
    toolButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* menu = new QMenu(toolButton_);
    toolButton_->setMenu(menu);
    bar->addWidget(toolButton_);
    auto* group = new QActionGroup(this);
    for (const auto id : kToolActions) {
        const auto* entry = editor::actionRegistry().find(id);
        auto* action = new QAction(fromStd(entry->name), this);
        action->setObjectName(fromStd(entry->id));
        action->setToolTip(fromStd(entry->description));
        action->setCheckable(true);
        group->addAction(action);
        editorActions_.emplace(entry->id, action);
        viewMenu_->addAction(action);
        menu->addAction(action);
        connect(action, &QAction::triggered, this, [this, id] { session_.perform(std::string(id)); });
    }
    auto* vertexModes = new QActionGroup(this);
    for (const char* id : {"vertex.mode.vertex", "vertex.mode.edge", "vertex.mode.face"}) {
        const auto* entry = editor::actionRegistry().find(id);
        auto* action = new QAction(fromStd(entry->name), this);
        action->setObjectName(fromStd(entry->id));
        action->setToolTip(fromStd(entry->description));
        action->setCheckable(true);
        vertexModes->addAction(action);
        editorActions_.emplace(entry->id, action);
        auto* button = new QToolButton(bar);
        button->setDefaultAction(action);
        toolOptions_[editor::EditorSession::Tool::Vertex].push_back(
            static_cast<QWidgetAction*>(bar->addWidget(button)));
        connect(action, &QAction::triggered, this, [this, id] {
            session_.perform(id);
            updateEditorActions();
        });
    }
    entityClass_ = new QComboBox(bar);
    entityClass_->setObjectName(QStringLiteral("entityClassPicker"));
    entityClass_->setEditable(true);
    entityClass_->setInsertPolicy(QComboBox::NoInsert);
    entityClass_->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    entityClass_->completer()->setFilterMode(Qt::MatchContains);
    entityClass_->setEnabled(false);
    entityClass_->setMinimumContentsLength(20);
    toolOptions_[editor::EditorSession::Tool::Entity].push_back(static_cast<QWidgetAction*>(bar->addWidget(entityClass_)));
    connect(entityClass_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        session_.setCurrentEntityClass(name.toStdString());
        if (entityClass_->findText(name) >= 0) QSettings().setValue(QStringLiteral("editor/entityClass"), name);
    });
    alignToSurface_ = new QCheckBox(tr("Align to surface"), bar);
    alignToSurface_->setObjectName(QStringLiteral("entityAlignToSurface"));
    toolOptions_[editor::EditorSession::Tool::Entity].push_back(static_cast<QWidgetAction*>(bar->addWidget(alignToSurface_)));
    connect(alignToSurface_, &QCheckBox::toggled, this, [this](bool align) { session_.setAlignToSurface(align); });
}

void MainWindow::loadEntityClasses() {
    assets::EntityClassTable table;
    if (const auto* profile = currentProfile(); profile && !profile->fgds.empty()) {
        if (auto loaded = assets::loadEntityClasses(profile->fgds)) table = std::move(*loaded);
        else log::warn("FGD load failed: {}", loaded.error().what());
    }
    session_.setEntityClasses(std::move(table));
}

void MainWindow::refreshEntityUi() {
    if (!entityClass_) return;
    const QSignalBlocker classBlock(entityClass_);
    if (shownClassesRevision_ != session_.entityClassesRevision()) {
        shownClassesRevision_ = session_.entityClassesRevision();
        QStringList names;
        for (const auto& [key, cls] : session_.entityClasses().classes)
            if (ciEquals(cls.type, "PointClass")) names.push_back(fromStd(cls.name));
        auto name = fromStd(session_.currentEntityClass());
        if (!names.contains(name)) name = QSettings().value(QStringLiteral("editor/entityClass")).toString();
        if (!names.contains(name)) name = names.value(0);
        entityClass_->clear();
        entityClass_->addItems(names);
        entityClass_->setCurrentIndex(static_cast<int>(names.indexOf(name)));
        entityClass_->setEditText(name);
        if (session_.placementModel().empty()) session_.setCurrentEntityClass(name.toStdString());
    }
    const auto item = session_.placementModel().empty() ? fromStd(session_.currentEntityClass())
        : tr("Model: %1").arg(fromStd(session_.placementModel()));
    if (entityClass_->currentText() != item) {
        entityClass_->setCurrentIndex(-1);
        entityClass_->setEditText(item);
    }
    entityClass_->setEnabled(entityClass_->count() != 0);
    const QSignalBlocker alignBlock(alignToSurface_);
    alignToSurface_->setChecked(session_.alignToSurface());
}

QString MainWindow::fragmentMimeType() { return QStringLiteral("application/x-apex-vmf-fragment"); }

std::string MainWindow::clipboardFragment() {
    const auto* data = QApplication::clipboard()->mimeData();
    if (!data) return {};
    if (data->hasFormat(fragmentMimeType())) {
        const QByteArray bytes = data->data(fragmentMimeType());
        const std::string text(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        if (looksLikeFragment(text)) return text;
    }
    if (data->hasText()) {
        const std::string text = data->text().toStdString();
        if (looksLikeFragment(text)) return text;
    }
    return {};
}

void MainWindow::refreshClipboardState() {
    clipboardHasFragment_ = !clipboardFragment().empty();
    updateEditorActions();
}

editor::PasteOptions MainWindow::pastePlacement(bool inPlace) const {
    editor::PasteOptions options;
    options.inPlace = true;
    if (inPlace || !viewport_) return options;
    const auto ray = viewport_->pointerRay();
    if (!ray) return options;
    const auto resolved = session_.resolvedWorkPlane();
    const auto plane = resolved ? resolved->plane : session_.fallbackPlane();
    const double denom = glm::dot(ray->dir, plane.normal);
    if (std::abs(denom) < 1e-9) return options;
    const double t = glm::dot(plane.origin - ray->origin, plane.normal) / denom;
    if (!std::isfinite(t) || t <= 0.0) return options;
    options.inPlace = false;
    options.at = ray->at(t);
    options.plane = plane;
    return options;
}

bool MainWindow::performEditAction(const editor::ActionId& id) {
    const auto say = [this](const QString& text) { statusBar()->showMessage(text, 5000); };
    if (id == "assets.reload") { reloadGameContent(); return true; }
    if (id == "compile.compile_run" || id == "compile.run_game") {
        showCompilation();
        if (id == "compile.run_game") compilePanel_->runGame();
        else compilePanel_->compileAndRun();
        return true;
    }
    if (!session_.document()) return false;
    bool ok = true;
    if (id == "edit.copy" || id == "edit.cut") {
        auto text = id == "edit.copy" ? session_.copyText() : session_.cutText();
        if (!text) {
            say(tr("%1: %2").arg(id == "edit.copy" ? tr("Copy") : tr("Cut"), QString::fromStdString(text.error().what())));
            ok = false;
        } else {
            auto* mime = new QMimeData;
            mime->setData(fragmentMimeType(), QByteArray(text->data(), static_cast<qsizetype>(text->size())));
            mime->setText(QString::fromStdString(*text));
            QApplication::clipboard()->setMimeData(mime);
            say(id == "edit.copy" ? tr("Copied") : tr("Cut"));
        }
    } else if (id == "edit.paste" || id == "edit.paste_in_place") {
        const auto text = clipboardFragment();
        if (text.empty()) {
            say(tr("The clipboard holds no VMF fragment."));
            ok = false;
        } else if (auto done = session_.pasteText(text, pastePlacement(id == "edit.paste_in_place")); !done) {
            say(tr("Paste: %1").arg(QString::fromStdString(done.error().what())));
            ok = false;
        }
    } else if (id == "edit.paste_special") {
        openPasteSpecial();
    } else if (id == "edit.tie_to_entity") {
        openTieDialog();
    } else if (id == "edit.create_prefab") {
        openPrefabDialog();
    } else {
        ok = session_.perform(id);
        if (!ok) say((id != "edit.carve" && id != "edit.hollow" && id != "edit.merge") || session_.topologyMessage().empty() ?
            tr("%1: nothing to do").arg(fromStd(editor::actionRegistry().find(id)->name)) :
            QString::fromStdString(session_.topologyMessage()));
    }
    updateEditorActions();
    return ok;
}

void MainWindow::openPasteSpecial() {
    if (!document_) return;
    if (auto* open = findChild<PasteSpecialDialog*>(QStringLiteral("pasteSpecialDialog"))) {
        open->raise();
        open->activateWindow();
        return;
    }
    const auto text = clipboardFragment();
    if (text.empty()) {
        statusBar()->showMessage(tr("The clipboard holds no VMF fragment."), 5000);
        return;
    }
    auto* dialog = new PasteSpecialDialog(session_, text, pastePlacement(false), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

QStringList MainWindow::solidClasses() {
    QStringList classes;
    for (const auto& [name, cls] : session_.entityClasses().classes)
        if (ciEquals(cls.type, "SolidClass")) classes.push_back(fromStd(cls.name));
    return classes;
}

void MainWindow::openTieDialog() {
    if (!document_) return;
    if (auto* open = findChild<TieDialog*>(QStringLiteral("tieDialog"))) {
        open->raise();
        open->activateWindow();
        return;
    }
    auto* dialog = new TieDialog(session_, solidClasses(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::createInstanceFromSelection() {
    if (!document_ || !gameContent_->profile()) return;
    const auto crossings = session_.instanceBoundaryConnections();
    if (!crossings.empty()) {
        QStringList lines;
        for (const auto& crossing : crossings) lines << QString::fromStdString(crossing);
        if (QMessageBox::warning(this, tr("Connections cross the selection boundary"),
                tr("These connections will keep their target names:\n%1\n\nCreate the instance?").arg(lines.join('\n')),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    }
    const auto target = compilePanel_->addonTarget();
    const auto root = target.empty() ? gameContent_->profile()->instanceSearchRoot() : target / "mapsrc";
    const auto chosen = QFileDialog::getSaveFileName(this, tr("Create Instance from Selection"),
        QString::fromStdU16String((root / "instances" / "selection.vmf").u16string()), tr("VMF maps (*.vmf)"));
    if (chosen.isEmpty()) return;
    const auto file = std::filesystem::path(chosen.toStdU16String());
    // A source-relative reference is understood by both vbsp and the editor; unsaved maps use the profile root.
    const auto base = session_.connectionMapPath().empty() ? root : session_.connectionMapPath().parent_path();
    const auto reference = file.lexically_relative(base).generic_string();
    auto result = session_.createInstanceFromSelection(file, reference.empty() ? file.generic_string() : reference, true);
    statusBar()->showMessage(result ? tr("Created instance %1").arg(chosen) : QString::fromStdString(result.error().what()), 5000);
    updateEditorActions();
}

void MainWindow::openPrefabDialog() {
    if (!document_) return;
    if (auto* open = findChild<PrefabDialog*>(QStringLiteral("prefabDialog"))) {
        open->raise();
        open->activateWindow();
        return;
    }
    const auto folder = gameContent_->prefabFolder();
    if (folder.empty()) {
        statusBar()->showMessage(tr("No prefab folder: pick a game profile or set one in Preferences."), 5000);
        return;
    }
    if (editor::selectedNodes(session_.selection()).empty()) {
        statusBar()->showMessage(tr("Create Prefab: nothing is selected."), 5000);
        return;
    }
    QStringList categories;
    for (const auto& entry : editor::listPrefabs(folder)) categories << QString::fromStdString(entry.category);
    auto* dialog = new PrefabDialog(session_, folder, categories, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &PrefabDialog::created, this, [this](const QString& file) {
        statusBar()->showMessage(tr("Created prefab %1").arg(file), 5000);
        gameContent_->notifyPrefabsChanged();
    });
    dialog->show();
}

void MainWindow::buildClipboardUi() {
    auto* edit = findChild<QMenu*>(QStringLiteral("menuEdit"));
    edit->addSeparator();
    for (const char* id : {"edit.cut", "edit.copy", "edit.paste", "edit.paste_in_place", "edit.paste_special",
                           "edit.duplicate", "edit.delete", "edit.group", "edit.ungroup", "edit.tie_to_entity",
                           "edit.move_to_world", "edit.carve", "edit.hollow", "edit.merge", "edit.create_prefab"}) {
        const auto* entry = editor::actionRegistry().find(id);
        auto* action = edit->addAction(fromStd(entry->name));
        action->setObjectName(fromStd(entry->id));
        action->setToolTip(fromStd(entry->description));
        editorActions_.emplace(entry->id, action);
        connect(action, &QAction::triggered, this, [this, id = entry->id] { performEditAction(id); });
        if (std::string_view(id) == "edit.duplicate" || std::string_view(id) == "edit.delete" ||
            std::string_view(id) == "edit.paste_special" || std::string_view(id) == "edit.ungroup")
            edit->addSeparator();
    }
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &MainWindow::refreshClipboardState);
    connect(edit, &QMenu::aboutToShow, this, &MainWindow::refreshClipboardState);
    clipboardHasFragment_ = !clipboardFragment().empty();
}

void MainWindow::centerViewsOnSelection() {
    if (const auto bounds = session_.selectionBounds(); bounds && viewport_) viewport_->centerOn(*bounds);
}

void MainWindow::openTransformDialog() {
    if (!document_) return;
    if (auto* open = findChild<TransformDialog*>(QStringLiteral("transformDialog"))) {
        open->raise();
        open->activateWindow();
        return;
    }
    auto* dialog = new TransformDialog(session_, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::restoreLayout() {
    QSettings s;
    if (const auto geo = s.value(QStringLiteral("ui/geometry")).toByteArray(); !geo.isEmpty()) restoreGeometry(geo);
    // Workspaces are per user; a missing or unreadable value gives the built-in set. The pre-0.2.12
    // keys (ui/docks, ui/viewports, splitter states) are no longer read.
    workspaces_->load();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings s;
    s.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    workspaces_->save();
    s.setValue(QStringLiteral("ui/theme"), currentThemeId());
    QMainWindow::closeEvent(event);
    if (event->isAccepted()) {
        autosaveTimer_->stop();
        autosave_.reset();
    }
}

void MainWindow::setTheme(Theme theme) {
    setThemeId(theme == Theme::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
}

void MainWindow::setThemeId(const QString& id) {
    applyThemeId(*qApp, id);
    refreshThemeMenu();
    session_.setOverlayPalette(overlayPalette(currentTokens()));
    viewport_->setThemeChanged();
}

void MainWindow::previewTokens(const Tokens& tokens) {
    applyTokens(*qApp, tokens, currentThemeId());
    session_.setOverlayPalette(overlayPalette(currentTokens()));
    viewport_->setThemeChanged();
}

void MainWindow::refreshThemeMenu() {
    themeMenu_->clear();
    auto* group = new QActionGroup(themeMenu_);
    bool separated = false;
    for (const ThemeEntry& t : themes()) {
        if (!t.builtin && !separated) {
            themeMenu_->addSeparator();
            separated = true;
        }
        auto* a = themeMenu_->addAction(t.name, this, [this, id = t.id] { setThemeId(id); });
        a->setCheckable(true);
        a->setChecked(t.id == currentThemeId());
        a->setObjectName(QStringLiteral("theme_%1").arg(t.id));
        group->addAction(a);
    }
}

void MainWindow::setProfiles(std::vector<assets::GameProfile> profiles) {
    const QString previous = profileCombo_->isEnabled() ? profileCombo_->currentText()
                                                        : QSettings().value(QStringLiteral("profile/last")).toString();
    profiles_ = std::move(profiles);
    const QSignalBlocker block(profileCombo_);
    profileCombo_->clear();
    for (const auto& p : profiles_) profileCombo_->addItem(QString::fromStdString(p.name));
    if (profiles_.empty()) {
        profileCombo_->addItem(tr("(no game profiles)"));
        profileCombo_->setEnabled(false);
        loadEntityClasses();
        session_.setConnectionContext(std::filesystem::path(path_.toStdU16String()));
        reloadGameContent();
        return;
    }
    profileCombo_->setEnabled(true);
    if (const int i = profileCombo_->findText(previous); i >= 0) profileCombo_->setCurrentIndex(i);
    loadEntityClasses();
    const auto* profile = currentProfile();
    session_.setConnectionContext(std::filesystem::path(path_.toStdU16String()),
        profile ? std::optional(profile->instanceSearchRoot()) : std::nullopt);
    reloadGameContent();
}

void MainWindow::reloadProfiles() {
    std::vector<assets::GameProfile> list;
    const QDir dir(profileDir());
    for (const auto& name : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        auto loaded = assets::loadGameProfile(std::filesystem::path(dir.filePath(name).toStdString()));
        if (loaded) list.push_back(std::move(*loaded));
        else log::warn("profile {}: {}", name.toStdString(), loaded.error().what());
    }
    if (auto p2ce = assets::detectP2ce()) {
        const bool known = std::ranges::any_of(list, [&](const auto& p) { return p.name == p2ce->name; });
        if (!known) list.push_back(std::move(*p2ce));
    }
    setProfiles(std::move(list));
    if (profiles_.empty()) log::warn("no game profiles found (looked in {} and for a P2CE install)", profileDir().toStdString());
}

const assets::GameProfile* MainWindow::currentProfile() const {
    const int i = profileCombo_->currentIndex();
    if (!profileCombo_->isEnabled() || i < 0 || static_cast<std::size_t>(i) >= profiles_.size()) return nullptr;
    return &profiles_[static_cast<std::size_t>(i)];
}

bool MainWindow::openFile(const QString& path) {
    return loadFile(path, path, false);
}

bool MainWindow::loadFile(const QString& readPath, const QString& path, bool recovered) {
    QFile f(readPath);
    if (!f.open(QIODevice::ReadOnly)) {
        const auto msg = tr("Cannot open %1: %2").arg(path, f.errorString());
        log::error("{}", msg.toStdString());
        QMessageBox::warning(this, tr("Open"), msg);
        return false;
    }
    const QByteArray bytes = f.readAll();
    vmf::ParseOptions options;
    if (const auto* profile = currentProfile()) options.escapeMode = profile->escapeMode;
    auto parsed = vmf::parse(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())), options);
    if (!parsed) {
        const auto msg = tr("Cannot parse %1: %2").arg(path, QString::fromStdString(parsed.error().what()));
        log::error("{}", msg.toStdString());
        QMessageBox::warning(this, tr("Open"), msg);
        return false;
    }
    installDocument(std::make_unique<document::Document>(*parsed), *parsed, path, recovered);
    if (!recovered) {
        addRecent(path);
        log::info("opened {}", path.toStdString());
        statusBar()->showMessage(tr("Opened %1").arg(path), 5000);
    }
    return true;
}

void MainWindow::installDocument(std::unique_ptr<document::Document> doc, const vmf::VmfFile& parsed,
                                 const QString& path, bool unsaved) {
    autosave_.reset();
    closeMaterialPicker();
    viewport_->setDocument(nullptr);
    session_.detach();
    compilePanel_->setDocument(nullptr);
    document_ = std::move(doc);
    document_->setUndoCap(static_cast<std::size_t>(undoCapMiB()) * 1024 * 1024);
    session_.attach(*document_);
    path_ = path;
    compilePanel_->setDocument(document_.get());
    compilePanel_->setSourcePath(std::filesystem::path(path_.toStdU16String()));
    const auto* profile = currentProfile();
    session_.setConnectionContext(std::filesystem::path(path_.toStdU16String()),
        profile ? std::optional(profile->instanceSearchRoot()) : std::nullopt);
    unsaved_ = unsaved;
    savedState_ = document_->historyState();
    viewport_->setDocument(document_.get(), currentProfile());
    viewport_->applyMapViews(viewport::readMapViews(parsed));
    saveAction_->setEnabled(true);
    saveAsAction_->setEnabled(true);
    startAutosave();
    updateTitle();
}

void MainWindow::newFile() {
    const auto empty = vmf::emptyMap();
    installDocument(std::make_unique<document::Document>(empty), empty, {}, true);
}

void MainWindow::startAutosave() {
    try {
        autosave_ = std::make_unique<editor::Autosave>(std::filesystem::path(path_.toStdU16String()));
    } catch (const std::exception& e) {
        log::warn("autosave: {}", e.what());
        statusBar()->showMessage(tr("Autosave unavailable: %1").arg(QString::fromUtf8(e.what())), 10000);
    }
}

void MainWindow::snapshotAutosave() {
    if (!document_ || !autosave_) return;
    if (auto done = autosave_->snapshot(*document_); !done) {
        log::warn("autosave: {}", done.error().what());
        statusBar()->showMessage(tr("Autosave failed: %1").arg(QString::fromStdString(done.error().what())), 10000);
    }
}

bool MainWindow::recoverAutosave(const editor::Recovery& recovery) {
    if (!loadFile(QString::fromStdU16String(recovery.snapshot.u16string()),
                  QString::fromStdU16String(recovery.source.u16string()), true)) return false;
    // Preserve recovery across a second crash, before retiring the stale session.
    if (!autosave_) return false;
    auto started = autosave_->snapshot(*document_);
    return started && autosave_->wait().has_value();
}

void MainWindow::offerRecovery() {
    try {
        auto entries = editor::findRecoveries();
        if (!entries) { log::warn("recovery: {}", entries.error().what()); return; }
        if (entries->empty()) return;
        auto* dialog = new RecoveryDialog(std::move(*entries), [this](const editor::Recovery& entry) {
            return recoverAutosave(entry);
        }, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    } catch (const std::exception& e) { log::warn("recovery: {}", e.what()); }
}

void MainWindow::configureAutosave() {
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("autosaveSettingsDialog"));
    dialog->setWindowTitle(tr("Autosave settings"));
    auto* form = new QFormLayout(dialog);
    auto* enabled = new QCheckBox(tr("Enable autosave"), dialog);
    enabled->setChecked(QSettings().value("autosave/enabled", true).toBool());
    auto* interval = new QSpinBox(dialog);
    interval->setRange(1, 1440);
    interval->setSuffix(tr(" min"));
    interval->setValue(QSettings().value("autosave/intervalMinutes", 5).toInt());
    form->addRow(enabled);
    form->addRow(tr("Interval"), interval);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, enabled, interval] {
        setAutosave(enabled->isChecked(), interval->value());
        dialog->accept();
    });
    dialog->show();
}

bool MainWindow::saveFile(const QString& path) {
    if (!document_) return false;
    auto text = document_->write();
    if (!text) {
        const auto msg = tr("Cannot serialise: %1").arg(QString::fromStdString(text.error().what()));
        log::error("{}", msg.toStdString());
        QMessageBox::warning(this, tr("Save"), msg);
        return false;
    }
    // Write the current camera views back into viewsettings (plan D19). Only values that changed
    // are touched; a map with no viewsettings block (Unsupported) is saved without views.
    if (viewport_) {
        vmf::ParseOptions options;
        if (const auto* profile = currentProfile()) options.escapeMode = profile->escapeMode;
        if (auto reparsed = vmf::parse(*text, options)) {
            if (auto wrote = viewport::writeMapViews(*reparsed, viewport_->captureMapViews()); wrote) {
                text = vmf::write(*reparsed);
            } else if (wrote.error().code != ErrorCode::Unsupported) {
                log::warn("map views not saved: {}", wrote.error().what());
            }
        }
    }
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) ||
        out.write(text->data(), static_cast<qint64>(text->size())) != static_cast<qint64>(text->size()) ||
        !out.commit()) {
        const auto msg = tr("Cannot write %1: %2").arg(path, out.errorString());
        log::error("{}", msg.toStdString());
        QMessageBox::warning(this, tr("Save"), msg);
        return false;
    }
    if (path_ != path) {
        autosave_.reset();
        path_ = path;
        compilePanel_->setSourcePath(std::filesystem::path(path_.toStdU16String()));
        const auto* profile = currentProfile();
        session_.setConnectionContext(std::filesystem::path(path_.toStdU16String()),
            profile ? std::optional(profile->instanceSearchRoot()) : std::nullopt);
        startAutosave();
    }
    unsaved_ = false;
    savedState_ = document_->historyState();
    setWindowModified(false);
    addRecent(path);
    updateTitle();
    log::info("saved {}", path.toStdString());
    statusBar()->showMessage(tr("Saved %1").arg(path), 5000);
    return true;
}

void MainWindow::openDialog() {
    const QString start = path_.isEmpty() ? QString() : QFileInfo(path_).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Open VMF"), start, tr("Valve Map (*.vmf);;All files (*)"));
    if (!path.isEmpty()) openFile(path);
}

void MainWindow::save() {
    if (path_.isEmpty()) saveAs();
    else saveFile(path_);
}

void MainWindow::saveAs() {
    QString start = path_;
    if (compilePanel_ && !compilePanel_->addonTarget().empty())
        start = QString::fromStdU16String((compilePanel_->addonTarget() / "mapsrc" /
            (path_.isEmpty() ? std::filesystem::path("untitled.vmf") : std::filesystem::path(QFileInfo(path_).fileName().toStdU16String()))).u16string());
    const QString path = QFileDialog::getSaveFileName(this, tr("Save VMF"), start, tr("Valve Map (*.vmf)"));
    if (!path.isEmpty()) saveFile(path);
}

QStringList MainWindow::recentFiles() const {
    return QSettings().value(QStringLiteral("recent/files")).toStringList();
}

void MainWindow::addRecent(const QString& path) {
    QStringList list = recentFiles();
    const QString abs = QFileInfo(path).absoluteFilePath();
    list.removeAll(abs);
    list.prepend(abs);
    while (list.size() > kMaxRecent) list.removeLast();
    QSettings().setValue(QStringLiteral("recent/files"), list);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    recentMenu_->clear();
    const QStringList list = recentFiles();
    for (const QString& p : list) {
        recentMenu_->addAction(p, this, [this, p] { openFile(p); });
    }
    if (list.isEmpty()) recentMenu_->addAction(tr("(none)"))->setEnabled(false);
    else {
        recentMenu_->addSeparator();
        recentMenu_->addAction(tr("Clear list"), this, [this] {
            QSettings().remove(QStringLiteral("recent/files"));
            rebuildRecentMenu();
        });
    }
}

void MainWindow::updateTitle() {
    const auto& v = version();
    QString title = QStringLiteral("Apex %1").arg(fromStd(v.string));
    if (!path_.isEmpty()) title = QStringLiteral("%1 - %2").arg(QFileInfo(path_).fileName(), title);
    if (document_ && (unsaved_ || savedState_ != document_->historyState())) title.prepend(QStringLiteral("* "));
    setWindowTitle(title);
}

// Monochrome SVGs tinted with the theme's text colour (D22); re-rendered when the theme changes.
void MainWindow::refreshSelectionIcons() {
    const std::pair<const char*, const char*> icons[] = {
        {"selection.mode.groups", "groups"}, {"selection.mode.objects", "objects"},
        {"selection.mode.solids", "solids"}, {"selection.mode.faces", "faces"},
        {"selection.ignore_groups", "ignore_groups"}, {"selection.marquee_toggle", "marquee"}};
    for (const auto& [id, name] : icons) {
        const auto found = editorActions_.find(id);
        if (found == editorActions_.end()) continue;
        QSvgRenderer renderer(QStringLiteral(":/icons/%1.svg").arg(QString::fromLatin1(name)));
        const auto dpr = devicePixelRatioF();
        QPixmap pixmap(QSize(24, 24) * dpr);
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(QRect(0, 0, 24, 24), currentTokens().text);
        painter.end();
        found->second->setIcon(QIcon(pixmap));
    }
}

bool MainWindow::event(QEvent* event) {
    if (event->type() == QEvent::ApplicationPaletteChange) refreshSelectionIcons();
    return QMainWindow::event(event);
}

}  // namespace apex::ui
