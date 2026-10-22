#include "ui/main_window.hpp"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QComboBox>
#include <QCheckBox>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>
#include <tuple>

#include "assets/fgd.hpp"
#include "core/kv1.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/version.hpp"
#include "document/document.hpp"
#include "document/vmf.hpp"
#include "ui/log_panel.hpp"
#include "ui/key_chord.hpp"
#include "ui/outliner_panel.hpp"
#include "ui/visgroup_panel.hpp"
#include "ui/history_panel.hpp"
#include "ui/inspector_panel.hpp"
#include "ui/paste_special_dialog.hpp"
#include "ui/tie_dialog.hpp"
#include "ui/transform_dialog.hpp"
#include "ui/viewport_area.hpp"
#include "viewport/map_views.hpp"

namespace apex::ui {
namespace {

constexpr int kMaxRecent = 10;

QString fromStd(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

bool isTransformAction(const editor::ActionId& id) {
    return id.starts_with("gizmo.") || id.starts_with("nudge.") || id.starts_with("edit.flip.") ||
           id == "transform.dialog" || id == "transform.texture_lock.toggle" || id == "snap.mode.toggle";
}

// Actions whose plain (unmodified) default keys belong to the focused pane.
bool isPaneKeyAction(const editor::ActionId& id) {
    return isTransformAction(id) || id.starts_with("view.") || id == "edit.delete" || id == "edit.duplicate";
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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    resize(1280, 800);
    buildMenus();
    buildStatusBar();
    buildTransformUi();
    buildClipboardUi();
    buildEntityUi();
    buildDocks();
    viewport_->setEditorSession(&session_);
    outliner_->setEditorSession(&session_);
    visgroups_->setEditorSession(&session_);
    history_->setEditorSession(&session_);
    inspector_->setEditorSession(&session_);
    sessionObserver_ = session_.observe([this] {
        updateEditorActions();
        updateGridStatus();
        refreshEntityUi();
    });
    const auto presetName = QSettings().value(QStringLiteral("editor/preset"), QStringLiteral("Hammer")).toString();
    const bool touching = QSettings().value(QStringLiteral("editor/marqueeTouching"), false).toBool();
    session_.setMarqueeMode(touching ? editor::MarqueeMode::Touching : editor::MarqueeMode::Enclosed);
    const auto* preset = editor::findPreset(presetName.toStdString());
    applyPreset(preset ? *preset : editor::hammerPreset());
    updateTitle();
    restoreLayout();
}

MainWindow::~MainWindow() {
    session_.unobserve(sessionObserver_);
    viewport_->setEditorSession(nullptr);
    outliner_->setEditorSession(nullptr);
    visgroups_->setEditorSession(nullptr);
    history_->setEditorSession(nullptr);
    inspector_->setEditorSession(nullptr);
    session_.detach();
    // The viewport area holds a raw pointer to the document.
    if (viewport_) viewport_->setDocument(nullptr);
    if (outliner_) outliner_->setDocument(nullptr);
    if (visgroups_) visgroups_->setDocument(nullptr);
    if (history_) history_->setDocument(nullptr);
    if (inspector_) inspector_->setDocument(nullptr);
}

void MainWindow::buildMenus() {
    auto* file = menuBar()->addMenu(tr("&File"));
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
    rebuildRecentMenu();

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    edit->setObjectName(QStringLiteral("menuEdit"));
    auto* selectionBar = addToolBar(tr("Selection"));
    selectionBar->setObjectName(QStringLiteral("toolbarSelection"));
    auto* modes = new QActionGroup(this);
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
            selectionBar->addAction(action);
        }
        connect(action, &QAction::triggered, this, [this, id = entry.id] {
            session_.perform(id);
            updateEditorActions();
        });
    }
    auto* settings = menuBar()->addMenu(tr("&Settings"));
    auto* presets = settings->addMenu(tr("Binding preset"));
    auto* presetGroup = new QActionGroup(presets);
    for (const auto name : editor::presetNames()) {
        auto* action = presets->addAction(fromStd(name));
        action->setObjectName(QStringLiteral("preset.") + fromStd(name));
        action->setCheckable(true);
        presetGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, name] { applyPreset(*editor::findPreset(name)); });
    }

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

    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Apex"), this, [this] {
        const auto& v = version();
        QMessageBox::about(this, tr("About Apex"),
                           tr("Apex %1\nA Source engine map editor.\nQt %2 (LGPL-3.0), Qt-ADS (LGPL-2.1).")
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
        log::info("game profile: {}", profiles_[static_cast<std::size_t>(i)].name);
    });
}

void MainWindow::applyPreset(const editor::Preset& preset) {
    session_.setPreset(preset);
    // Native pane key delivery uses this same session keymap.
    for (const auto& [id, action] : editorActions_) {
        QList<QKeySequence> sequences;
        const auto& binding = session_.preset().bindings.at(id);
        const bool transformAction = isPaneKeyAction(id);
        if (const auto* chords = std::get_if<std::vector<editor::KeyChord>>(&binding.value)) {
            for (const auto& chord : *chords) {
                // A bare modifier is a held key (snap.suspend style), never a shortcut.
                if (isTransformAction(id) && (chord.key == "Ctrl" || chord.key == "Alt" || chord.key == "Shift")) continue;
                sequences.push_back(keySequence(chord));
            }
        }
        action->setShortcuts(sequences);
        // Unmodified keys (W/E/R, arrows, Shift+L) belong to the focused pane, which routes them
        // through the same keymap; a window shortcut would steal them from the outliner.
        if (transformAction) {
            bool plain = true;
            if (const auto* chords = std::get_if<std::vector<editor::KeyChord>>(&binding.value)) {
                for (const auto& chord : *chords) plain = plain && !chord.ctrl && !chord.alt;
            }
            action->setShortcutContext(plain ? Qt::WidgetShortcut : Qt::WindowShortcut);
        }
    }
    for (const auto name : editor::presetNames()) {
        if (auto* action = findChild<QAction*>(QStringLiteral("preset.") + fromStd(name)))
            action->setChecked(name == preset.name);
    }
    QSettings().setValue(QStringLiteral("editor/preset"), fromStd(preset.name));
}

void MainWindow::updateEditorActions() {
    for (const auto& [id, action] : editorActions_) {
        action->setEnabled(session_.document() != nullptr);
        if (id == "tool.select" || id == "tool.entity") {
            action->setEnabled(true);
            const auto tool = id == "tool.select" ? editor::EditorSession::Tool::Select : editor::EditorSession::Tool::Entity;
            action->setChecked(session_.activeTool() == tool);
        }
        if (id.starts_with("edit.") && !id.starts_with("edit.flip.")) {
            const bool have = session_.document() != nullptr;
            const bool objects = have && !editor::selectedNodes(session_.selection()).empty();
            if (id == "edit.copy" || id == "edit.cut" || id == "edit.delete" || id == "edit.duplicate" ||
                id == "edit.group" || id == "edit.ungroup" || id == "edit.tie_to_entity" || id == "edit.move_to_world")
                action->setEnabled(objects);
            if (id == "edit.paste" || id == "edit.paste_in_place" || id == "edit.paste_special")
                action->setEnabled(have && clipboardHasFragment_);
        }
        if (id == "view.hide" || id == "view.hide_unselected" || id == "view.isolate") {
            const bool objects = session_.document() && !editor::selectedNodes(session_.selection()).empty();
            action->setEnabled(objects || (id == "view.isolate" && session_.viewState().isolate.has_value()));
        }
        if (id == "view.isolate") action->setChecked(session_.viewState().isolate.has_value());
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
        if (id == "snap.mode.toggle") {
            const bool relative = session_.snapSettings().mode == editor::SnapMode::Relative;
            action->setChecked(relative);
            action->setText(relative ? tr("Snap: Relative") : tr("Snap: Absolute"));
        }
        if (id == "transform.texture_lock.toggle") action->setChecked(session_.textureLock());
        const std::pair<const char*, editor::SelectionMode> modes[] = {
            {"selection.mode.groups", editor::SelectionMode::Groups},
            {"selection.mode.objects", editor::SelectionMode::Objects},
            {"selection.mode.solids", editor::SelectionMode::Solids},
            {"selection.mode.faces", editor::SelectionMode::Faces}};
        for (const auto& [modeId, mode] : modes) {
            if (id == modeId) action->setChecked(session_.selection().mode() == mode);
        }
    }
}

void MainWindow::buildDocks() {
    // Side panels only (D15/D16). Floating is off: panels are plain widgets, but a floating
    // window is the case the Wayland spike showed crashing, so keep everything in the main window.
    using ads::CDockManager;
    CDockManager::setConfigFlags(CDockManager::DefaultNonOpaqueConfig);
    CDockManager::setConfigFlag(CDockManager::OpaqueSplitterResize, true);
    CDockManager::setConfigFlag(CDockManager::DockAreaHasUndockButton, false);
    CDockManager::setConfigFlag(CDockManager::DockAreaHasTabsMenuButton, false);
    CDockManager::setConfigFlag(CDockManager::HideSingleCentralWidgetTitleBar, true);
    CDockManager::setConfigFlag(CDockManager::FloatingContainerForceNativeTitleBar, true);

    // The central widget is a plain splitter: [panels | viewports] over [log]. The left
    // panels share an ADS manager; the viewports are never inside ADS.
    auto* vertical = new QSplitter(Qt::Vertical);
    vertical->setObjectName(QStringLiteral("splitMain"));
    auto* horizontal = new QSplitter(Qt::Horizontal);
    horizontal->setObjectName(QStringLiteral("splitTop"));
    horizontal->setChildrenCollapsible(false);
    vertical->setChildrenCollapsible(false);

    dockManager_ = new CDockManager;
    dockManager_->setStyleSheet(QString());  // Use the application QSS (theme tokens).
    logDockManager_ = new CDockManager;
    logDockManager_->setStyleSheet(QString());

    viewport_ = new ViewportArea;
    horizontal->addWidget(dockManager_);
    horizontal->addWidget(viewport_);
    horizontal->setStretchFactor(0, 0);
    horizontal->setStretchFactor(1, 1);
    horizontal->setSizes({260, 1000});
    vertical->addWidget(horizontal);
    vertical->addWidget(logDockManager_);
    vertical->setStretchFactor(0, 1);
    vertical->setStretchFactor(1, 0);
    vertical->setSizes({700, 160});
    setCentralWidget(vertical);

    outliner_ = new OutlinerPanel;
    auto* outlinerDock = new ads::CDockWidget(dockManager_, tr("Outliner"));
    outlinerDock->setObjectName(QStringLiteral("dockOutliner"));
    outlinerDock->setWidget(outliner_);
    outlinerDock->setFeature(ads::CDockWidget::DockWidgetFloatable, false);
    outlinerDock->setFeature(ads::CDockWidget::DockWidgetClosable, true);
    auto* leftArea = dockManager_->addDockWidget(ads::CenterDockWidgetArea, outlinerDock);
    visgroups_ = new VisgroupPanel;
    history_ = new HistoryPanel;
    inspector_ = new InspectorPanel;
    std::vector<ads::CDockWidget*> leftDocks{outlinerDock};
    for (const auto& [name, title, panel] : std::vector<std::tuple<const char*, QString, QWidget*>>{
             {"dockInspector", tr("Inspector"), inspector_}, {"dockVisgroups", tr("Visgroups"), visgroups_},
             {"dockHistory", tr("Undo History"), history_}}) {
        auto* dock = new ads::CDockWidget(dockManager_, title);
        dock->setObjectName(QString::fromLatin1(name));
        dock->setWidget(panel);
        dock->setFeature(ads::CDockWidget::DockWidgetFloatable, false);
        dockManager_->addDockWidget(ads::CenterDockWidgetArea, dock, leftArea);
        leftDocks.push_back(dock);
        viewMenu_->addAction(dock->toggleViewAction());
    }
    for (auto* dock : leftDocks) {
        connect(dock, &ads::CDockWidget::viewToggled, this, [this, leftDocks] {
            const bool open = std::any_of(leftDocks.begin(), leftDocks.end(), [](auto* d) { return !d->isClosed(); });
            dockManager_->setVisible(open);
        });
    }

    log_ = new LogPanel;
    auto* logDock = new ads::CDockWidget(logDockManager_, tr("Log"));
    logDock->setObjectName(QStringLiteral("dockLog"));
    logDock->setWidget(log_);
    logDock->setFeature(ads::CDockWidget::DockWidgetFloatable, false);
    logDockManager_->addDockWidget(ads::CenterDockWidgetArea, logDock);
    connect(logDock, &ads::CDockWidget::viewToggled, logDockManager_, &QWidget::setVisible);

    viewMenu_->addAction(outlinerDock->toggleViewAction());
    viewMenu_->addAction(logDock->toggleViewAction());
    viewMenu_->addSeparator();
    auto* themes = new QActionGroup(this);
    darkAction_ = viewMenu_->addAction(tr("Dark theme"), this, [this] { setTheme(Theme::Dark); });
    lightAction_ = viewMenu_->addAction(tr("Light theme"), this, [this] { setTheme(Theme::Light); });
    for (auto* a : {darkAction_, lightAction_}) {
        a->setCheckable(true);
        themes->addAction(a);
    }
    darkAction_->setObjectName(QStringLiteral("actionDark"));
    lightAction_->setObjectName(QStringLiteral("actionLight"));
    (currentTheme() == Theme::Dark ? darkAction_ : lightAction_)->setChecked(true);
    viewMenu_->addSeparator();
    viewMenu_->addAction(tr("Reset cameras"), viewport_, &ViewportArea::resetCameras);
    viewMenu_->addAction(tr("Reset viewport layout"), viewport_, &ViewportArea::resetLayout);

    connect(viewport_, &ViewportArea::cameraPositionChanged, this, [this](double x, double y, double z) {
        cameraLabel_->setText(tr("Cam: %1 %2 %3").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1).arg(z, 0, 'f', 1));
    });
    connect(viewport_, &ViewportArea::frameTimeChanged, this,
            [this](double ms) { frameLabel_->setText(tr("%1 ms").arg(ms, 0, 'f', 1)); });
    connect(viewport_, &ViewportArea::gpuNameChanged, this,
            [this](const QString& n) { gpuLabel_->setText(tr("GPU: %1").arg(n)); });
}

void MainWindow::updateGridStatus() {
    const auto grid = tr("Grid: %1").arg(session_.gridSize(), 0, 'g', 12);
    const auto resolved = session_.resolvedWorkPlane();
    const auto plane = !resolved ? tr("Plane: ground")
        : resolved->source == editor::WorkPlaneSource::Pinned ? tr("Plane: pinned") : tr("Plane: face (hover)");
    if (gridLabel_->text() != grid) gridLabel_->setText(grid);
    if (planeLabel_->text() != plane) planeLabel_->setText(plane);
    const auto readout = session_.transformActive() ? QString::fromStdString(session_.activeReadout().text) : QString();
    if (readoutLabel_->text() != readout) readoutLabel_->setText(readout);
}

void MainWindow::buildStatusBar() {
    gridLabel_ = new QLabel;
    gridLabel_->setObjectName(QStringLiteral("statusGrid"));
    planeLabel_ = new QLabel;
    planeLabel_->setObjectName(QStringLiteral("statusPlane"));
    readoutLabel_ = new QLabel;
    readoutLabel_->setObjectName(QStringLiteral("statusReadout"));
    statusBar()->addWidget(readoutLabel_);
    cameraLabel_ = new QLabel(tr("Cam: -"));
    frameLabel_ = new QLabel(tr("- ms"));
    gpuLabel_ = new QLabel(tr("GPU: -"));
    for (auto* l : {gridLabel_, planeLabel_, cameraLabel_, frameLabel_, gpuLabel_}) statusBar()->addPermanentWidget(l);
    updateGridStatus();
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
            if (id == "transform.dialog") openTransformDialog();
            else session_.perform(id);
            updateEditorActions();
        });
        return action;
    };

    viewMenu_->addSeparator();
    for (const char* id : {"view.hide", "view.unhide", "view.isolate", "view.hide_unselected"})
        make(id, viewMenu_, std::string_view(id) == "view.isolate");
    viewMenu_->addSeparator();

    auto* edit = findChild<QMenu*>(QStringLiteral("menuEdit"));
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
    auto* snap = make("snap.mode.toggle", gizmo, true);

    auto* bar = addToolBar(tr("Transform"));
    bar->setObjectName(QStringLiteral("toolbarTransform"));
    for (auto* action : modeActions) bar->addAction(action);
    bar->addSeparator();
    bar->addAction(snap);
    bar->addAction(textureLock);
    bar->addAction(space);

    // The same actions as checkable buttons in the status bar (always visible, even with the
    // toolbar hidden).
    const std::pair<const char*, QAction*> buttons[] = {
        {"statusGizmoTranslate", modeActions[0]}, {"statusGizmoRotate", modeActions[1]},
        {"statusGizmoScale", modeActions[2]}, {"statusSnapMode", snap}, {"statusTextureLock", textureLock}};
    for (const auto& [name, action] : buttons) {
        auto* button = new QToolButton;
        button->setObjectName(QString::fromLatin1(name));
        button->setDefaultAction(action);
        button->setAutoRaise(true);
        statusBar()->addPermanentWidget(button);
    }
}

void MainWindow::buildEntityUi() {
    auto* bar = findChild<QToolBar*>(QStringLiteral("toolbarTransform"));
    bar->addSeparator();
    auto* group = new QActionGroup(this);
    for (const char* id : {"tool.select", "tool.entity"}) {
        const auto* entry = editor::actionRegistry().find(id);
        auto* action = new QAction(fromStd(entry->name), this);
        action->setObjectName(fromStd(entry->id));
        action->setCheckable(true);
        group->addAction(action);
        editorActions_.emplace(entry->id, action);
        viewMenu_->addAction(action);
        bar->addAction(action);
        connect(action, &QAction::triggered, this, [this, id] { session_.perform(id); });
    }
    entityClass_ = new QComboBox(bar);
    entityClass_->setObjectName(QStringLiteral("entityClassPicker"));
    entityClass_->setEditable(true);
    entityClass_->setInsertPolicy(QComboBox::NoInsert);
    entityClass_->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    entityClass_->completer()->setFilterMode(Qt::MatchContains);
    entityClass_->setEnabled(false);
    entityClass_->setMinimumContentsLength(20);
    bar->addWidget(entityClass_);
    connect(entityClass_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        session_.setCurrentEntityClass(name.toStdString());
        if (entityClass_->findText(name) >= 0) QSettings().setValue(QStringLiteral("editor/entityClass"), name);
    });
    alignToSurface_ = new QCheckBox(tr("Align to surface"), bar);
    alignToSurface_->setObjectName(QStringLiteral("entityAlignToSurface"));
    bar->addWidget(alignToSurface_);
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
        session_.setCurrentEntityClass(name.toStdString());
    }
    if (entityClass_->currentText() != fromStd(session_.currentEntityClass()))
        entityClass_->setEditText(fromStd(session_.currentEntityClass()));
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
    } else {
        ok = session_.perform(id);
        if (!ok) say(tr("%1: nothing to do").arg(fromStd(editor::actionRegistry().find(id)->name)));
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

void MainWindow::buildClipboardUi() {
    auto* edit = findChild<QMenu*>(QStringLiteral("menuEdit"));
    edit->addSeparator();
    for (const char* id : {"edit.cut", "edit.copy", "edit.paste", "edit.paste_in_place", "edit.paste_special",
                           "edit.duplicate", "edit.delete", "edit.group", "edit.ungroup", "edit.tie_to_entity",
                           "edit.move_to_world"}) {
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
    if (const auto st = s.value(QStringLiteral("ui/docks")).toByteArray(); !st.isEmpty()) dockManager_->restoreState(st);
    if (const auto st = s.value(QStringLiteral("ui/docksLog")).toByteArray(); !st.isEmpty()) logDockManager_->restoreState(st);
    for (const char* name : {"splitMain", "splitTop"}) {
        auto* splitter = findChild<QSplitter*>(QString::fromLatin1(name));
        const auto st = s.value(QStringLiteral("ui/") + QString::fromLatin1(name)).toByteArray();
        if (splitter && !st.isEmpty()) splitter->restoreState(st);
    }
    // The viewport layout is per user; a missing or unreadable value keeps the default quad.
    viewport_->restoreLayoutJson(s.value(QStringLiteral("ui/viewports")).toByteArray());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings s;
    s.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    s.setValue(QStringLiteral("ui/docks"), dockManager_->saveState());
    s.setValue(QStringLiteral("ui/docksLog"), logDockManager_->saveState());
    for (const char* name : {"splitMain", "splitTop"})
        if (auto* splitter = findChild<QSplitter*>(QString::fromLatin1(name)))
            s.setValue(QStringLiteral("ui/") + QString::fromLatin1(name), splitter->saveState());
    s.setValue(QStringLiteral("ui/viewports"), viewport_->layoutJson());
    s.setValue(QStringLiteral("ui/theme"), currentTheme() == Theme::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
    QMainWindow::closeEvent(event);
}

void MainWindow::setTheme(Theme theme) {
    applyTheme(*qApp, theme);
    dockManager_->setStyleSheet(QString());
    logDockManager_->setStyleSheet(QString());
    (theme == Theme::Dark ? darkAction_ : lightAction_)->setChecked(true);
    viewport_->setThemeChanged();
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
        return;
    }
    profileCombo_->setEnabled(true);
    if (const int i = profileCombo_->findText(previous); i >= 0) profileCombo_->setCurrentIndex(i);
    loadEntityClasses();
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
    QFile f(path);
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
    auto doc = std::make_unique<document::Document>(*parsed);
    viewport_->setDocument(nullptr);
    outliner_->setDocument(nullptr);
    visgroups_->setDocument(nullptr);
    history_->setDocument(nullptr);
    inspector_->setDocument(nullptr);
    session_.detach();
    document_ = std::move(doc);
    session_.attach(*document_);
    path_ = path;
    viewport_->setDocument(document_.get(), currentProfile());
    viewport_->applyMapViews(viewport::readMapViews(*parsed));
    outliner_->setDocument(document_.get());
    visgroups_->setDocument(document_.get());
    history_->setDocument(document_.get());
    inspector_->setDocument(document_.get());
    saveAction_->setEnabled(true);
    saveAsAction_->setEnabled(true);
    addRecent(path);
    updateTitle();
    log::info("opened {}", path.toStdString());
    statusBar()->showMessage(tr("Opened %1").arg(path), 5000);
    return true;
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
    path_ = path;
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
    const QString path = QFileDialog::getSaveFileName(this, tr("Save VMF"), path_, tr("Valve Map (*.vmf)"));
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
    setWindowTitle(title);
}

}  // namespace apex::ui
