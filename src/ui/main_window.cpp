#include "ui/main_window.hpp"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>

#include "core/log.hpp"
#include "core/version.hpp"
#include "document/document.hpp"
#include "document/vmf.hpp"
#include "ui/log_panel.hpp"
#include "ui/key_chord.hpp"
#include "ui/outliner_panel.hpp"
#include "ui/viewport_area.hpp"
#include "viewport/map_views.hpp"

namespace apex::ui {
namespace {

constexpr int kMaxRecent = 10;

QString fromStd(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
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
    buildDocks();
    viewport_->setEditorSession(&session_);
    outliner_->setEditorSession(&session_);
    sessionObserver_ = session_.observe([this] {
        updateEditorActions();
        updateGridStatus();
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
    session_.detach();
    // The viewport area holds a raw pointer to the document.
    if (viewport_) viewport_->setDocument(nullptr);
    if (outliner_) outliner_->setDocument(nullptr);
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
        log::info("game profile: {}", profiles_[static_cast<std::size_t>(i)].name);
    });
}

void MainWindow::applyPreset(const editor::Preset& preset) {
    session_.setPreset(preset);
    // SelectToolActive is always active until tools land. All shortcuts come from
    // the registry; native pane key delivery uses this same session Keymap.
    for (const auto& [id, action] : editorActions_) {
        QList<QKeySequence> sequences;
        const auto& binding = session_.preset().bindings.at(id);
        if (const auto* chords = std::get_if<std::vector<editor::KeyChord>>(&binding.value)) {
            for (const auto& chord : *chords) sequences.push_back(keySequence(chord));
        }
        action->setShortcuts(sequences);
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

    // The central widget is a plain splitter: [outliner | viewports] over [log]. Each side panel
    // is its own ADS manager holding one dock widget; the viewports are never inside ADS.
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
    dockManager_->addDockWidget(ads::CenterDockWidgetArea, outlinerDock);
    connect(outlinerDock, &ads::CDockWidget::viewToggled, dockManager_, &QWidget::setVisible);

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
}

void MainWindow::buildStatusBar() {
    gridLabel_ = new QLabel;
    gridLabel_->setObjectName(QStringLiteral("statusGrid"));
    planeLabel_ = new QLabel;
    planeLabel_->setObjectName(QStringLiteral("statusPlane"));
    cameraLabel_ = new QLabel(tr("Cam: -"));
    frameLabel_ = new QLabel(tr("- ms"));
    gpuLabel_ = new QLabel(tr("GPU: -"));
    for (auto* l : {gridLabel_, planeLabel_, cameraLabel_, frameLabel_, gpuLabel_}) statusBar()->addPermanentWidget(l);
    updateGridStatus();
    statusBar()->showMessage(tr("Ready"));
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
        return;
    }
    profileCombo_->setEnabled(true);
    if (const int i = profileCombo_->findText(previous); i >= 0) profileCombo_->setCurrentIndex(i);
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
    session_.detach();
    document_ = std::move(doc);
    session_.attach(*document_);
    path_ = path;
    viewport_->setDocument(document_.get(), currentProfile());
    viewport_->applyMapViews(viewport::readMapViews(*parsed));
    outliner_->setDocument(document_.get());
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
