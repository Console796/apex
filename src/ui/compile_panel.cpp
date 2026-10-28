#include "ui/compile_panel.hpp"

#include "compile/wc_import.hpp"
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
    row->addWidget(profilesCombo_);
    run_ = new QPushButton(tr("Run"), this);
    run_->setObjectName(QStringLiteral("compileRun"));
    cancel_ = new QPushButton(tr("Cancel"), this);
    cancel_->setObjectName(QStringLiteral("compileCancel"));
    auto* import = new QPushButton(tr("Import .wc…"), this);
    row->addWidget(run_); row->addWidget(cancel_); row->addWidget(import);
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
    log_ = new QPlainTextEdit(this);
    log_->setObjectName(QStringLiteral("compileLog"));
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(10000);
    layout->addWidget(log_);
    connect(run_, &QPushButton::clicked, this, &CompilePanel::run);
    connect(cancel_, &QPushButton::clicked, this, &CompilePanel::cancel);
    connect(import, &QPushButton::clicked, this, &CompilePanel::importProfiles);
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
}

} // namespace apex::ui
