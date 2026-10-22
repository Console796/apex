#include "ui/history_panel.hpp"

#include <QLabel>
#include <QListWidget>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace apex::ui {

HistoryPanel::HistoryPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    memory_ = new QLabel(this);
    memory_->setObjectName(QStringLiteral("historyMemory"));
    layout->addWidget(memory_);
    list_ = new QListWidget(this);
    layout->addWidget(list_);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (session_ && document_ && session_->document() == document_) {
            const auto target = item->data(Qt::UserRole).toULongLong();
            session_->jumpHistory(static_cast<std::size_t>(target));
        }
    });
    refresh();
}

HistoryPanel::~HistoryPanel() {
    setEditorSession(nullptr);
    if (document_) document_->unobserve(documentObserver_);
}

void HistoryPanel::setDocument(document::Document* document) {
    if (document_) document_->unobserve(documentObserver_);
    document_ = document;
    if (document_) documentObserver_ = document_->observe([this](const document::ChangeSet&) { refresh(); });
    refresh();
}

void HistoryPanel::setEditorSession(editor::EditorSession* session) {
    if (session_) session_->unobserve(sessionObserver_);
    session_ = session;
    if (session_) sessionObserver_ = session_->observe([this] {
        list_->setEnabled(document_ && session_->document() == document_);
    });
    refresh();
}

void HistoryPanel::refresh() {
    const QSignalBlocker blocker(list_);
    list_->clear();
    const auto count = document_ ? document_->undoCount() + document_->redoCount() : 0;
    const double bytes = document_ ? static_cast<double>(document_->historyBytes()) : 0;
    const bool mib = bytes >= 1024 * 1024;
    const double cap = document_ ? static_cast<double>(document_->undoCap()) / (1024 * 1024) : 0;
    memory_->setText(tr("%1 entries · %2 %3 / %4 MiB").arg(count).arg(bytes / (mib ? 1024 * 1024 : 1024), 0, 'f', 1)
                        .arg(mib ? tr("MiB") : tr("KiB")).arg(cap, 0, 'f', 0));
    list_->setEnabled(document_ && session_ && session_->document() == document_);
    if (!document_) return;
    const auto add = [this](const QString& label, std::size_t target) {
        auto* item = new QListWidgetItem(label, list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(target));
        return item;
    };
    add(tr("Initial state"), 0);
    std::size_t target = 0;
    for (const auto& label : document_->undoLabels()) add(QString::fromStdString(label), ++target);
    auto* current = add(tr("→ Current state"), target);
    auto font = current->font();
    font.setBold(true);
    current->setFont(font);
    list_->setCurrentItem(current);
    for (const auto& label : document_->redoLabels()) add(QString::fromStdString(label), ++target);
    list_->scrollToItem(current);
}

} // namespace apex::ui
