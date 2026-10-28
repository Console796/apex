#include "ui/compile_panel.hpp"
#include "ui/asset_report_builder.hpp"

#include "compile/wc_import.hpp"
#include "ui/cordon_editor.hpp"
#include "ui/profile_editor.hpp"
#include "ui/profile_store.hpp"
#include "compile/game_launch.hpp"
#include "compile/map_files.hpp"
#include "editor/compile_overlay.hpp"
#include "editor/diagnostic_lookup.hpp"
#include "ui/game_content.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QShowEvent>
#include <fstream>

namespace apex::ui {

CompilePanelView::CompilePanelView(CompilePanel* panel, QWidget* owner, QWidget* parent)
    : QWidget(parent), panel_(panel), owner_(owner) {
    if (!panel_) { panel_ = new CompilePanel(this); owner_ = this; }
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll_);
    placeholder_ = new QWidget(this);
    auto* message = new QVBoxLayout(placeholder_);
    auto* text = new QLabel(tr("Compilation is displayed in another area or window."));
    text->setWordWrap(true);
    message->addWidget(text);
    auto* show = new QPushButton(tr("Show compilation here"));
    message->addWidget(show);
    message->addStretch();
    layout->addWidget(placeholder_);
    connect(show, &QPushButton::clicked, this, &CompilePanelView::activate);
    scroll_->hide();
}

CompilePanelView::~CompilePanelView() { release(); }

void CompilePanelView::release() {
    if (!panel_ || panel_->view_ != this) return;
    scroll_->takeWidget();
    panel_->hide();
    panel_->setParent(owner_);
    panel_->view_ = nullptr;
    scroll_->hide();
    placeholder_->show();
}

void CompilePanelView::activate() {
    if (!panel_ || panel_->view_ == this) return;
    if (panel_->view_) panel_->view_->release();
    panel_->view_ = this;
    scroll_->setWidget(panel_);
    placeholder_->hide();
    scroll_->show();
    panel_->show();
}

void CompilePanelView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    activate();
}

namespace {
QString statusText(compile::Status status) {
    switch (status) {
    case compile::Status::Queued: return QStringLiteral("Queued");
    case compile::Status::Running: return QStringLiteral("Running");
    case compile::Status::Ok: return QStringLiteral("Ok");
    case compile::Status::Failed: return QStringLiteral("Failed");
    case compile::Status::Cancelled: return QStringLiteral("Cancelled");
    case compile::Status::Skipped: return QStringLiteral("Skipped");
    }
    return {};
}
QString specialText(compile::SpecialCommand command) {
    switch (command) {
    case compile::SpecialCommand::ChangeDirectory: return QStringLiteral("Change directory");
    case compile::SpecialCommand::Copy: return QStringLiteral("Copy");
    case compile::SpecialCommand::Delete: return QStringLiteral("Delete");
    case compile::SpecialCommand::Rename: return QStringLiteral("Rename");
    case compile::SpecialCommand::CopyIfExists: return QStringLiteral("Copy if exists");
    }
    return {};
}
}

CompilePanel::CompilePanel(QWidget* parent) : QWidget(parent), profiles_(compile::builtinProfiles()) {
    cache_ = std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::CacheLocation).toStdU16String());
    auto* layout = new QVBoxLayout(this);
    auto* row = new QHBoxLayout;
    profilesCombo_ = new QComboBox(this);
    profilesCombo_->setObjectName(QStringLiteral("compileProfile"));
    for (auto& profile : profiles_) {
        profile.runGameAfter = false;
        profilesCombo_->addItem(QString::fromStdString(profile.name));
    }
    builtinCount_ = static_cast<int>(profiles_.size());
    for (auto& profile : loadUserProfiles()) {
        profilesCombo_->addItem(QString::fromStdString(profile.name));
        profiles_.push_back(std::move(profile));
    }
    row->addWidget(profilesCombo_);
    run_ = new QPushButton(tr("Compile"), this);
    run_->setObjectName(QStringLiteral("compileRun"));
    cancel_ = new QPushButton(tr("Cancel"), this);
    cancel_->setObjectName(QStringLiteral("compileCancel"));
    auto* import = new QPushButton(tr("Import .wc…"), this);
    auto* edit = new QPushButton(tr("Edit…"), this);
    edit->setObjectName(QStringLiteral("compileEdit"));
    auto* remove = new QPushButton(tr("Delete"), this);
    remove->setObjectName(QStringLiteral("compileDelete"));
    compileRun_ = new QPushButton(tr("Compile and Run"), this);
    compileRun_->setObjectName(QStringLiteral("compileAndRun"));
    runGame_ = new QPushButton(tr("Run Game"), this);
    runGame_->setObjectName(QStringLiteral("compileRunGame"));
    row->addWidget(run_); row->addWidget(compileRun_); row->addWidget(runGame_); row->addWidget(cancel_); row->addWidget(edit); row->addWidget(remove); row->addWidget(import);
    layout->addLayout(row);
    header_ = new QLabel(this);
    header_->setObjectName(QStringLiteral("compileHeader"));
    header_->setWordWrap(true);
    layout->addWidget(header_);
    steps_ = new QTableWidget(0, 2, this);
    steps_->setObjectName(QStringLiteral("compileSteps"));
    steps_->setHorizontalHeaderLabels({tr("Step"), tr("Status")});
    steps_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    steps_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(steps_);
    auto* overlayRow = new QHBoxLayout;
    showLeak_ = new QCheckBox(tr("Leak"), this);
    showLeak_->setObjectName(QStringLiteral("compileShowLeak"));
    showLeak_->setChecked(true);
    showPortals_ = new QCheckBox(tr("Portals"), this);
    showPortals_->setObjectName(QStringLiteral("compileShowPortals"));
    showPortals_->setChecked(true);
    cluster_ = new QComboBox(this);
    cluster_->setObjectName(QStringLiteral("compileCluster"));
    cluster_->addItem(tr("All clusters"), -1);
    goToLeak_ = new QPushButton(tr("Go to leak start"), this);
    goToLeak_->setObjectName(QStringLiteral("compileGoToLeak"));
    goToLeak_->setEnabled(false);
    overlayRow->addWidget(showLeak_);
    overlayRow->addWidget(showPortals_);
    overlayRow->addWidget(cluster_);
    overlayRow->addWidget(goToLeak_);
    overlayRow->addStretch(1);
    layout->addLayout(overlayRow);
    cordons_ = new CordonEditor(this);
    cordons_->setObjectName(QStringLiteral("cordonEditor"));
    layout->addWidget(cordons_);
    connect(showLeak_, &QCheckBox::toggled, this, [this] { overlayControlsChanged(); });
    connect(showPortals_, &QCheckBox::toggled, this, [this] { overlayControlsChanged(); });
    connect(cluster_, &QComboBox::currentIndexChanged, this, [this] { overlayControlsChanged(); });
    connect(goToLeak_, &QPushButton::clicked, this, [this] {
        if (session_ && session_->compileOverlay())
            if (const auto start = session_->compileOverlay()->leakStart()) emit focusRequested(*start);
    });
    auto* problemRow = new QHBoxLayout;
    auto* check = new QPushButton(tr("Check for problems"), this);
    check->setObjectName(QStringLiteral("problemsCheck"));
    problemFilter_ = new QComboBox(this);
    problemFilter_->setObjectName(QStringLiteral("problemsFilter"));
    problemFilter_->addItems({tr("All"), tr("Errors and warnings"), tr("Errors")});
    problemSummary_ = new QLabel(this);
    problemSummary_->setObjectName(QStringLiteral("problemsSummary"));
    problemRow->addWidget(check);
    problemRow->addWidget(problemFilter_);
    problemRow->addWidget(problemSummary_, 1);
    layout->addLayout(problemRow);
    problemsTable_ = new QTableWidget(0, 2, this);
    problemsTable_->setObjectName(QStringLiteral("problemsTable"));
    problemsTable_->setHorizontalHeaderLabels({tr("Severity"), tr("Problem")});
    problemsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    problemsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    problemsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(problemsTable_);
    connect(check, &QPushButton::clicked, this, [this] { checkProblems(); });
    connect(problemFilter_, &QComboBox::currentIndexChanged, this, [this] { refreshProblemList(); });
    connect(problemsTable_, &QTableWidget::cellClicked, this, [this](int row) { activateProblem(row); });
    auto* filterRow = new QHBoxLayout;
    severity_ = new QComboBox(this);
    severity_->setObjectName(QStringLiteral("compileSeverity"));
    severity_->addItems({tr("All"), tr("Errors and warnings"), tr("Errors")});
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("compileSummary"));
    filterRow->addWidget(severity_);
    filterRow->addWidget(summary_, 1);
    layout->addLayout(filterRow);
    diagnosticsTable_ = new QTableWidget(0, 3, this);
    diagnosticsTable_->setObjectName(QStringLiteral("compileDiagnostics"));
    diagnosticsTable_->setHorizontalHeaderLabels({tr("Severity"), tr("Tool"), tr("Message")});
    diagnosticsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    diagnosticsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    diagnosticsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(diagnosticsTable_);
    log_ = new QPlainTextEdit(this);
    log_->setObjectName(QStringLiteral("compileLog"));
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(10000);
    layout->addWidget(log_);
    auto* gameRow = new QHBoxLayout;
    reloadMap_ = new QPushButton(tr("Reload map"), this);
    reloadMap_->setObjectName(QStringLiteral("gameReloadMap"));
    reloadMap_->setToolTip(tr("Start the game again in the last compiled map."));
    gameRow->addWidget(new QLabel(tr("Game log"), this));
    gameRow->addStretch(1);
    gameRow->addWidget(reloadMap_);
    layout->addLayout(gameRow);
    gameLog_ = new QPlainTextEdit(this);
    gameLog_->setObjectName(QStringLiteral("gameLog"));
    gameLog_->setReadOnly(true);
    gameLog_->setMaximumBlockCount(10000);
    layout->addWidget(gameLog_);
    gameLogTimer_ = new QTimer(this);
    gameLogTimer_->setInterval(250);
    connect(gameLogTimer_, &QTimer::timeout, this, &CompilePanel::pollGameLog);
    connect(run_, &QPushButton::clicked, this, &CompilePanel::run);
    connect(compileRun_, &QPushButton::clicked, this, &CompilePanel::compileAndRun);
    connect(runGame_, &QPushButton::clicked, this, &CompilePanel::runGame);
    connect(reloadMap_, &QPushButton::clicked, this, &CompilePanel::runGame);
    connect(cancel_, &QPushButton::clicked, this, &CompilePanel::cancel);
    connect(import, &QPushButton::clicked, this, &CompilePanel::importProfiles);
    connect(severity_, &QComboBox::currentIndexChanged, this, [this] { diagnosticLines_ = static_cast<std::size_t>(-1); refreshDiagnostics(); });
    connect(diagnosticsTable_, &QTableWidget::cellClicked, this, [this](int row) { activateDiagnostic(row); });
    connect(edit, &QPushButton::clicked, this, &CompilePanel::editProfile);
    connect(remove, &QPushButton::clicked, this, &CompilePanel::deleteProfile);
    timer_ = new QTimer(this);
    timer_->setInterval(40);
    connect(timer_, &QTimer::timeout, this, &CompilePanel::refresh);
    timer_->start();
    refresh();
}

CompilePanel::~CompilePanel() = default;
void CompilePanel::setDocument(document::Document* document) {
    if (document_ == document) return;
    stopJob();
    job_.reset();
    revision_.reset();
    document_ = document;
    cordons_->setDocument(document);
    problems_.clear();
    problemsBefore_.reset();
    refreshProblemList();
    error_.clear();
    log_->clear();
    steps_->setRowCount(0);
    diagnostics_.clear();
    diagnosticLines_ = 0;
    refreshDiagnostics();
    refresh();
}
void CompilePanel::setEditorSession(editor::EditorSession* session) {
    session_ = session;
    cordons_->setEditorSession(session);
}
void CompilePanel::setProcessRunner(compile::ProcessRunner* runner) {
    stopJob();
    job_.reset();
    runner_ = runner ? runner : &realRunner_;
    refresh();
}
void CompilePanel::stopJob() {
    if (!job_ || job_->status() != compile::Status::Running) return;
    job_->cancel();
    // Leave the work directory alone until process-group escalation has completed.
    cancelling_ = true;
    QTimer::singleShot(2100, Qt::PreciseTimer, this, [this] { cancelling_ = false; refresh(); });
}
void CompilePanel::cancel() { stopJob(); refresh(); }

void CompilePanel::run() { startCompile(false); }
void CompilePanel::compileAndRun() { startCompile(true); }

void CompilePanel::startCompile(bool withGame) {
    if (cancelling_ || !document_ || !content_ || !content_->profile() ||
        (job_ && job_->status() == compile::Status::Running)) return;
    const int index = profilesCombo_->currentIndex();
    if (index < 0) return;
    auto profile = profiles_[static_cast<std::size_t>(index)];
    if (withGame) {
        profile.runGameAfter = true;
        const bool hasGame = std::ranges::any_of(profile.commands, [](const auto& c) { return c.executable == "$game_exe"; });
        if (!hasGame) profile.commands.push_back({"$game_exe", compile::defaultGameArguments(), true, false});
        for (auto& command : profile.commands) if (command.executable == "$game_exe") command.enabled = true;
    }
    if (index >= builtinCount_ && !confirmed_.contains(index)) {
        QStringList commands;
        for (const auto& executable : compile::listNonToolExecutables(profile))
            commands << QString::fromStdString(executable);
        for (const auto& command : profile.commands)
            if (command.special) commands << specialText(*command.special) + ": " + QString::fromStdString(command.arguments);
        if (QMessageBox::question(this, tr("Run imported profile?"),
            tr("This imported profile contains these non-tool executables and special commands:\n%1\n\nRun it?")
                .arg(commands.isEmpty() ? tr("(none)") : commands.join('\n')),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        confirmed_.insert(index);
    }
    checkProblems();
    const int errors = editor::countProblems(problems_, editor::ProblemSeverity::Error);
    const int warnings = editor::countProblems(problems_, editor::ProblemSeverity::Warning);
    if (errors > 0) {
        const bool proceed = gate_ ? gate_(errors, warnings)
            : QMessageBox::question(this, tr("Problems in the map"),
                  tr("The map has %1 error(s) and %2 warning(s). The compile will probably fail.\n\nCompile anyway?")
                      .arg(errors).arg(warnings),
                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
        if (!proceed) return;
    }
    problemsBefore_ = std::pair{errors, warnings};
    job_.reset();
    revision_.reset();
    steps_->setRowCount(0);
    error_.clear();
    log_->clear();
    diagnostics_.clear();
    diagnosticLines_ = 0;
    refreshDiagnostics();
    const auto& game = *content_->profile();
    editor::CompileGamePaths paths{game.gameDirectory, game.toolDirectory(), game.instanceSearchRoot(), {},
                                   content_->classes(), game.escapeMode};
    for (const auto& [variable, tool] : {std::pair{"bsp_exe", "vbsp"}, {"vis_exe", "vvis"},
             {"light_exe", "vrad"}, {"postcompiler_exe", "postcompiler"}, {"game_exe", "game"}})
        paths.tools[variable] = game.toolPath(tool).string();
    if (const auto vfs = content_->vfs(); vfs && std::ranges::any_of(vfs->addons().entries,
            [](const auto& addon) { return addon.mounted; })) {
        auto info = vfs->compileGameInfo();
        if (!info) { error_ = QString::fromStdString(info.error().what()); refresh(); return; }
        paths.contentGameInfo = std::move(*info);
    }
    auto snapshot = editor::writeCompileSnapshot(*document_, source_, cache_, paths,
        profile.cordonOnly ? std::optional(editor::CompileCordon{profile.cordonMaterial}) : std::nullopt);
    if (!snapshot) { error_ = QString::fromStdString(snapshot.error().what()); refresh(); return; }
    revision_ = snapshot->revision;
    overlayDirectory_ = snapshot->paths.workDirectory;
    overlayName_ = snapshot->paths.variables.at("file");
    overlayLoaded_ = false;
    if (session_) session_->setCompileOverlay(std::nullopt);
    cluster_->clear();
    cluster_->addItem(tr("All clusters"), -1);
    goToLeak_->setEnabled(false);
    if (profile.runGameAfter) watchGameLog();
    logged_.assign(profile.commands.size(), 0);
    steps_->setRowCount(static_cast<int>(profile.commands.size()));
    for (std::size_t i = 0; i < profile.commands.size(); ++i) {
        const auto& command = profile.commands[i];
        steps_->setItem(static_cast<int>(i), 0, new QTableWidgetItem(command.special ?
            specialText(*command.special) : QString::fromStdString(command.executable)));
        steps_->setItem(static_cast<int>(i), 1, new QTableWidgetItem);
    }
    job_ = std::make_unique<compile::CompileJob>(std::move(profile), std::move(snapshot->paths), *runner_, files_);
    if (auto started = job_->start(); !started) error_ = QString::fromStdString(started.error().what());
    refresh();
}

void CompilePanel::runGame() {
    if (!content_ || !content_->profile()) return;
    const auto& game = *content_->profile();
    compile::GameLaunchRequest request;
    request.gameExe = game.toolPath("game");
    request.gameDir = game.gameDirectory;
    request.mapName = source_.stem().string();
    const int index = profilesCombo_->currentIndex();
    if (index >= 0)
        for (const auto& command : profiles_[static_cast<std::size_t>(index)].commands)
            if (command.executable == "$game_exe") request.arguments = command.arguments;
    if (request.mapName.empty()) { error_ = tr("Save the map and compile it before running the game."); refresh(); return; }
    auto launch = compile::buildGameLaunch(request);
    if (!launch) { error_ = QString::fromStdString(launch.error().what()); refresh(); return; }
    std::error_code ec;
    if (!std::filesystem::exists(launch->bspFile, ec)) {
        error_ = tr("There is no compiled map to run: %1").arg(QString::fromStdU16String(launch->bspFile.u16string()));
        refresh();
        return;
    }
    watchGameLog();
    if (auto started = runner_->startDetached(launch->program, launch->arguments, game.gameDirectory.parent_path(), {}); !started) {
        error_ = QString::fromStdString(started.error().what());
        gameLogTimer_->stop();
    } else error_.clear();
    refresh();
}

// The log is appended to across launches unless the game clears it, so only what comes after this point is shown.
void CompilePanel::watchGameLog() {
    if (!content_ || !content_->profile()) return;
    gameLogFile_ = compile::gameLogPath(content_->profile()->gameDirectory);
    std::error_code ec;
    gameLogOffset_ = std::filesystem::exists(gameLogFile_, ec) ? std::filesystem::file_size(gameLogFile_, ec) : 0;
    gameLog_->clear();
    gameLogTimer_->start();
}

void CompilePanel::pollGameLog() {
    if (gameLogFile_.empty()) return;
    std::error_code ec;
    const auto size = std::filesystem::exists(gameLogFile_, ec) ? std::filesystem::file_size(gameLogFile_, ec) : 0;
    if (ec) return;
    if (size < gameLogOffset_) gameLogOffset_ = 0; // the game cleared the log
    if (size == gameLogOffset_) return;
    std::ifstream in(gameLogFile_, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(gameLogOffset_));
    std::string text(static_cast<std::size_t>(size - gameLogOffset_), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    gameLogOffset_ += text.size();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (!text.empty()) gameLog_->appendPlainText(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
}

QString CompilePanel::gameLogText() const { return gameLog_->toPlainText(); }

void CompilePanel::refresh() {
    const bool running = job_ && job_->status() == compile::Status::Running;
    const bool ready = document_ && content_ && content_->hasProfile() && !running && !cancelling_;
    run_->setEnabled(ready);
    compileRun_->setEnabled(ready);
    runGame_->setEnabled(content_ && content_->hasProfile() && !running);
    reloadMap_->setEnabled(content_ && content_->hasProfile() && !running);
    cancel_->setEnabled(running);
    profilesCombo_->setEnabled(!running);
    QString header = error_;
    if (revision_) {
        if (!header.isEmpty()) header += '\n';
        header += tr("Snapshot at revision %1%2").arg(*revision_).arg(
            document_ && document_->historyState() != *revision_ ? tr(" (stale)") : QString());
    }
    header_->setText(header);
    if (!job_) return;
    QStringList batch;
    std::size_t budget = 500;
    for (std::size_t i = 0; i < job_->steps().size(); ++i) {
        const auto& step = job_->steps()[i];
        auto* item = steps_->item(static_cast<int>(i), 1);
        item->setText(statusText(step.status));
        item->setToolTip(QString::fromStdString(step.message));
        while (logged_[i] < step.lines.size() && budget) {
            batch << QString::fromStdString(step.lines[logged_[i]++]);
            --budget;
        }
    }
    if (!batch.isEmpty()) log_->appendPlainText(batch.join('\n'));
    refreshDiagnostics();
    if (!overlayLoaded_ && job_->status() != compile::Status::Queued && job_->status() != compile::Status::Running)
        loadOverlay();
}

// A finished compile replaces the overlay: a fixed leak leaves no .lin, and the portals take its place.
void CompilePanel::loadOverlay() {
    overlayLoaded_ = true;
    if (!session_ || !revision_) return;
    auto overlay = editor::loadCompileOverlay(overlayDirectory_, overlayName_, *revision_);
    overlay.showLeak = showLeak_->isChecked();
    overlay.showPortals = showPortals_->isChecked();
    cluster_->blockSignals(true);
    for (int i = 0; i < overlay.portals.clusters; ++i) cluster_->addItem(tr("Cluster %1").arg(i), i);
    cluster_->blockSignals(false);
    goToLeak_->setEnabled(overlay.leakStart().has_value());
    if (overlay.leak.empty() && overlay.portals.portals.empty()) session_->setCompileOverlay(std::nullopt);
    else session_->setCompileOverlay(std::move(overlay));
}

void CompilePanel::overlayControlsChanged() {
    if (!session_ || !session_->compileOverlay()) return;
    auto overlay = *session_->compileOverlay();
    overlay.showLeak = showLeak_->isChecked();
    overlay.showPortals = showPortals_->isChecked();
    const int cluster = cluster_->currentData().toInt();
    overlay.cluster = cluster < 0 ? std::nullopt : std::optional<int>(cluster);
    session_->setCompileOverlay(std::move(overlay));
}

void CompilePanel::refreshDiagnostics() {
    std::size_t lines = 0;
    if (job_) for (const auto& step : job_->steps()) lines += step.lines.size() + (step.status == compile::Status::Running ? 0 : 1);
    if (lines == diagnosticLines_ && !(job_ == nullptr && diagnosticsTable_->rowCount())) return;
    diagnosticLines_ = lines;
    if (job_) diagnostics_ = compile::collectDiagnostics(job_->steps());
    shown_.clear();
    diagnosticsTable_->setRowCount(0);
    const int level = severity_->currentIndex();
    for (std::size_t i = 0; i < diagnostics_.size(); ++i) {
        const auto& d = diagnostics_[i];
        if (level == 1 && d.severity == compile::Severity::Info) continue;
        if (level == 2 && d.severity != compile::Severity::Error) continue;
        const int row = diagnosticsTable_->rowCount();
        diagnosticsTable_->insertRow(row);
        shown_.push_back(static_cast<int>(i));
        diagnosticsTable_->setItem(row, 0, new QTableWidgetItem(d.severity == compile::Severity::Error ? tr("Error") :
            d.severity == compile::Severity::Warning ? tr("Warning") : tr("Info")));
        diagnosticsTable_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(d.tool)));
        diagnosticsTable_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(d.text)));
    }
    if (!job_) { summary_->clear(); return; }
    const auto summary = compile::summarize(diagnostics_, job_->steps());
    QStringList parts;
    if (problemsBefore_)
        parts << tr("Problems before: %1 errors, %2 warnings").arg(problemsBefore_->first).arg(problemsBefore_->second);
    for (const auto& [tool, counts] : summary.tools)
        parts << tr("%1: %2 errors, %3 warnings").arg(QString::fromStdString(tool)).arg(counts.errors).arg(counts.warnings);
    if (summary.seconds) parts << tr("%1 s in tools").arg(summary.seconds);
    summary_->setText(parts.isEmpty() ? tr("No problems reported") : parts.join(QStringLiteral(" · ")));
}

const std::vector<editor::Problem>& CompilePanel::checkProblems() {
    problems_.clear();
    if (document_) problems_ = collectMapProblems(*document_, content_, session_);
    refreshProblemList();
    return problems_;
}
void CompilePanel::refreshProblemList() {
    shownProblems_.clear();
    problemsTable_->setRowCount(0);
    const int level = problemFilter_->currentIndex();
    for (std::size_t i = 0; i < problems_.size(); ++i) {
        const auto& p = problems_[i];
        if (level == 1 && p.severity == editor::ProblemSeverity::Note) continue;
        if (level == 2 && p.severity != editor::ProblemSeverity::Error) continue;
        const int row = problemsTable_->rowCount();
        problemsTable_->insertRow(row);
        shownProblems_.push_back(static_cast<int>(i));
        problemsTable_->setItem(row, 0, new QTableWidgetItem(tr(editor::severityName(p.severity))));
        problemsTable_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(p.text)));
    }
    problemSummary_->setText(problems_.empty() ? tr("No problems found") :
        tr("%1 errors, %2 warnings, %3 notes")
            .arg(editor::countProblems(problems_, editor::ProblemSeverity::Error))
            .arg(editor::countProblems(problems_, editor::ProblemSeverity::Warning))
            .arg(editor::countProblems(problems_, editor::ProblemSeverity::Note)));
}
void CompilePanel::activateProblem(int row) {
    if (row < 0 || row >= static_cast<int>(shownProblems_.size()) || !document_ || !session_) return;
    const auto& problem = problems_[static_cast<std::size_t>(shownProblems_[static_cast<std::size_t>(row)])];
    std::vector<editor::Target> targets;
    for (const auto id : problem.objects) if (document_->handle(id)) targets.emplace_back(id);
    if (targets.empty()) return;
    session_->selection().setMode(editor::SelectionMode::Objects);
    session_->selection().replace(targets);
    emit frameRequested();
}

void CompilePanel::activateDiagnostic(int row) {
    if (row < 0 || row >= static_cast<int>(shown_.size()) || !document_ || !session_) return;
    const auto& diagnostic = diagnostics_[static_cast<std::size_t>(shown_[static_cast<std::size_t>(row)])];
    if (diagnostic.objects.empty()) {
        if (diagnostic.position) emit focusRequested(DVec3((*diagnostic.position)[0], (*diagnostic.position)[1], (*diagnostic.position)[2]));
        return;
    }
    std::vector<editor::Target> targets;
    for (const auto& object : diagnostic.objects)
        if (const auto node = editor::findDiagnosticObject(*document_, object)) targets.emplace_back(*node);
    auto* message = diagnosticsTable_->item(row, 2);
    if (targets.empty()) {
        message->setToolTip(tr("No longer in the map"));
        summary_->setText(tr("%1 is no longer in the map").arg(message->text().section(':', 0, 0)));
        return;
    }
    message->setToolTip({});
    session_->selection().setMode(editor::SelectionMode::Objects);
    session_->selection().replace(targets);
    emit frameRequested();
}

void CompilePanel::importProfiles() {
    QString start;
    if (content_ && content_->profile()) start = QString::fromStdU16String(
        (content_->profile()->gameDirectory.parent_path() / "hammer/cfg/cmdseq.wc").u16string());
    const auto file = QFileDialog::getOpenFileName(this, tr("Import compile profiles"), start, tr("Hammer sequences (*.wc)"));
    if (file.isEmpty()) return;
    auto imported = compile::importWc(std::filesystem::path(file.toStdU16String()));
    if (!imported) { error_ = QString::fromStdString(imported.error().what()); refresh(); return; }
    for (auto& profile : *imported) {
        profilesCombo_->addItem(QString::fromStdString(profile.name));
        profiles_.push_back(std::move(profile));
    }
    saveProfiles();
}

void CompilePanel::saveProfiles() const {
    saveUserProfiles({profiles_.begin() + builtinCount_, profiles_.end()});
}

// A built-in profile is edited as a copy; the edit is a new user profile, selected on accept.
void CompilePanel::editProfile() {
    const int index = profilesCombo_->currentIndex();
    if (index < 0 || (job_ && job_->status() == compile::Status::Running)) return;
    auto profile = profiles_[static_cast<std::size_t>(index)];
    const bool builtin = index < builtinCount_;
    if (builtin) profile.name += " copy";
    ProfileEditor editor(std::move(profile), this);
    if (editor.exec() != QDialog::Accepted) return;
    auto edited = editor.profile();
    if (edited.name.empty()) edited.name = "Untitled";
    if (builtin) {
        profilesCombo_->addItem(QString::fromStdString(edited.name));
        profiles_.push_back(std::move(edited));
        profilesCombo_->setCurrentIndex(profileCount() - 1);
    } else {
        profilesCombo_->setItemText(index, QString::fromStdString(edited.name));
        profiles_[static_cast<std::size_t>(index)] = std::move(edited);
        confirmed_.erase(index);
    }
    saveProfiles();
}

void CompilePanel::deleteProfile() {
    const int index = profilesCombo_->currentIndex();
    if (index < builtinCount_ || (job_ && job_->status() == compile::Status::Running)) return;
    profiles_.erase(profiles_.begin() + index);
    profilesCombo_->removeItem(index);
    // Confirmations are keyed by index, so those after the removed profile shift down.
    std::set<int> shifted;
    for (const int confirmed : confirmed_) if (confirmed != index) shifted.insert(confirmed > index ? confirmed - 1 : confirmed);
    confirmed_ = std::move(shifted);
    saveProfiles();
}

} // namespace apex::ui
