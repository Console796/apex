#include "ui/compile_panel.hpp"

#include "compile/wc_import.hpp"
#include "ui/profile_editor.hpp"
#include "ui/profile_store.hpp"
#include "editor/diagnostic_lookup.hpp"
#include "ui/game_content.hpp"
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

namespace apex::ui {
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
        profile.runGameAfter = false;
        profilesCombo_->addItem(QString::fromStdString(profile.name));
        profiles_.push_back(std::move(profile));
    }
    row->addWidget(profilesCombo_);
    run_ = new QPushButton(tr("Run"), this);
    run_->setObjectName(QStringLiteral("compileRun"));
    cancel_ = new QPushButton(tr("Cancel"), this);
    cancel_->setObjectName(QStringLiteral("compileCancel"));
    auto* import = new QPushButton(tr("Import .wc…"), this);
    auto* edit = new QPushButton(tr("Edit…"), this);
    edit->setObjectName(QStringLiteral("compileEdit"));
    auto* remove = new QPushButton(tr("Delete"), this);
    remove->setObjectName(QStringLiteral("compileDelete"));
    row->addWidget(run_); row->addWidget(cancel_); row->addWidget(edit); row->addWidget(remove); row->addWidget(import);
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
    connect(run_, &QPushButton::clicked, this, &CompilePanel::run);
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
    error_.clear();
    log_->clear();
    steps_->setRowCount(0);
    diagnostics_.clear();
    diagnosticLines_ = 0;
    refreshDiagnostics();
    refresh();
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

void CompilePanel::run() {
    if (cancelling_ || !document_ || !content_ || !content_->profile() ||
        (job_ && job_->status() == compile::Status::Running)) return;
    const int index = profilesCombo_->currentIndex();
    if (index < 0) return;
    auto profile = profiles_[static_cast<std::size_t>(index)];
    profile.runGameAfter = false;
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
    job_.reset();
    revision_.reset();
    steps_->setRowCount(0);
    error_.clear();
    log_->clear();
    diagnostics_.clear();
    diagnosticLines_ = 0;
    refreshDiagnostics();
    const auto& game = *content_->profile();
    editor::CompileGamePaths paths{game.gameDirectory, game.toolDirectory(), game.instanceSearchRoot(), {}};
    for (const auto& [variable, tool] : {std::pair{"bsp_exe", "vbsp"}, {"vis_exe", "vvis"},
             {"light_exe", "vrad"}, {"postcompiler_exe", "postcompiler"}, {"game_exe", "game"}})
        paths.tools[variable] = game.toolPath(tool).string();
    auto snapshot = editor::writeCompileSnapshot(*document_, source_, cache_, paths);
    if (!snapshot) { error_ = QString::fromStdString(snapshot.error().what()); refresh(); return; }
    revision_ = snapshot->revision;
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

void CompilePanel::refresh() {
    const bool running = job_ && job_->status() == compile::Status::Running;
    run_->setEnabled(document_ && content_ && content_->hasProfile() && !running && !cancelling_);
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
    for (const auto& [tool, counts] : summary.tools)
        parts << tr("%1: %2 errors, %3 warnings").arg(QString::fromStdString(tool)).arg(counts.errors).arg(counts.warnings);
    if (summary.seconds) parts << tr("%1 s in tools").arg(summary.seconds);
    summary_->setText(parts.isEmpty() ? tr("No problems reported") : parts.join(QStringLiteral(" · ")));
}

void CompilePanel::activateDiagnostic(int row) {
    if (row < 0 || row >= static_cast<int>(shown_.size()) || !document_ || !session_) return;
    const auto& diagnostic = diagnostics_[static_cast<std::size_t>(shown_[static_cast<std::size_t>(row)])];
    if (diagnostic.objects.empty()) return;
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
        profile.runGameAfter = false;
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
    edited.runGameAfter = false;
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
