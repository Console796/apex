#include "ui/recovery_dialog.hpp"

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace apex::ui {

RecoveryDialog::RecoveryDialog(std::vector<editor::Recovery> recoveries, Open open, QWidget* parent)
    : QDialog(parent), recoveries_(std::move(recoveries)), open_(std::move(open)) {
    setObjectName(QStringLiteral("recoveryDialog"));
    setWindowTitle(tr("Recover maps"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Apex did not close cleanly. Recover an autosaved map?"), this));
    list_ = new QListWidget(this);
    for (const auto& entry : recoveries_) {
        auto label = entry.source.empty() ? tr("Untitled") : QString::fromStdU16String(entry.source.u16string());
        if (entry.snapshot.empty()) label += tr(" (no snapshot)");
        list_->addItem(label);
    }
    layout->addWidget(list_);
    for (const auto& [label, choice] : {std::pair{tr("Open snapshot"), Choice::Open},
                                      {tr("Discard"), Choice::Discard}, {tr("Keep for later"), Choice::Later}}) {
        auto* button = new QPushButton(label, this);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, choice] {
            const int row = list_->currentRow();
            if (row >= 0) choose(static_cast<std::size_t>(row), choice);
        });
    }
    if (!recoveries_.empty()) list_->setCurrentRow(0);
}

bool RecoveryDialog::choose(std::size_t index, Choice choice) {
    if (index >= recoveries_.size()) return false;
    const auto& entry = recoveries_[index];
    if (choice == Choice::Open && (entry.snapshot.empty() || !open_(entry))) return false;
    if (choice != Choice::Later) {
        auto done = editor::discardRecovery(entry);
        if (!done) return false;
    }
    recoveries_.erase(recoveries_.begin() + static_cast<std::ptrdiff_t>(index));
    delete list_->takeItem(static_cast<int>(index));
    if (recoveries_.empty() || choice == Choice::Open) accept();
    return true;
}

} // namespace apex::ui
