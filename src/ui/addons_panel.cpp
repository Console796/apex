#include "ui/addons_panel.hpp"

#include "assets/vfs.hpp"
#include "ui/game_content.hpp"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPixmap>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace apex::ui {
namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
QString joined(const std::vector<std::string>& values) {
    QStringList result;
    for (const auto& value : values) result.push_back(text(value));
    return result.join(QStringLiteral(", "));
}
}
AddonsPanel::AddonsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("AddonsPanel");
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    auto* actions = new QHBoxLayout;
    filter_ = new QLineEdit(this); filter_->setObjectName("addonsFilter");
    filter_->setPlaceholderText(tr("Filter addons"));
    auto* reload = new QPushButton(tr("Reload"), this); reload->setObjectName("addonsReload");
    actions->addWidget(filter_, 1); actions->addWidget(reload);
    root->addLayout(actions);
    entries_ = new QTreeWidget(this); entries_->setObjectName("addonsEntries");
    entries_->setHeaderLabels({tr("Mounted / Name"), tr("Authors"), tr("Type"), tr("Source"), tr("Workshop ID"), tr("Tags"), tr("Warnings")});
    entries_->setRootIsDecorated(false);
    entries_->setIconSize(QSize(64, 64));
    entries_->header()->setStretchLastSection(true);
    root->addWidget(entries_, 1);
    status_ = new QLabel(this); status_->setObjectName("addonsStatus"); status_->setWordWrap(true);
    root->addWidget(status_);
    auto* notice = new QLabel(tr("Hammer reads the addon list when it starts; restart Hammer for a change to apply"), this);
    notice->setObjectName("addonsRestartNotice"); notice->setWordWrap(true);
    root->addWidget(notice);
    connect(filter_, &QLineEdit::textChanged, this, &AddonsPanel::filter);
    connect(reload, &QPushButton::clicked, this, [this] {
        if (content_) { auto profile = content_->profile(); content_->reload(profile ? &*profile : nullptr); }
    });
    connect(entries_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (!content_ || column != 0) return;
        const auto index = static_cast<std::size_t>(item->data(0, Qt::UserRole).toULongLong());
        const auto mounted = item->checkState(0) == Qt::Checked;
        // Reload clears the rows; defer it until itemChanged has finished delivering.
        QMetaObject::invokeMethod(this, [this, index, mounted] {
            if (!content_) return;
            if (auto result = content_->setAddonMounted(index, mounted); !result) {
                refresh(); status_->setText(text(result.error().what()));
            }
        }, Qt::QueuedConnection);
    });
}
void AddonsPanel::setGameContent(GameContent* content) {
    if (content_) disconnect(content_, nullptr, this, nullptr);
    content_ = content;
    if (content_) connect(content_, &GameContent::contentChanged, this, &AddonsPanel::refresh);
    refresh();
}
void AddonsPanel::refresh() {
    const QSignalBlocker blocker(entries_);
    entries_->clear();
    status_->setText(content_ ? content_->error() : tr("No game profile"));
    if (!content_) return;
    if (content_->loading()) status_->setText(tr("Loading addons…"));
    const auto vfs = content_->vfs();
    if (!vfs) return;
    const auto& list = vfs->addons();
    if (status_->text().isEmpty()) status_->setText(joined(list.problems));
    for (std::size_t i = 0; i < list.entries.size(); ++i) {
        const auto& entry = list.entries[i];
        auto* row = new QTreeWidgetItem(entries_, {text(entry.name), joined(entry.authors), text(entry.addonType),
            entry.type == "steam" ? tr("Workshop") : tr("Local"), text(entry.workshopId), joined(entry.tags), joined(entry.problems)});
        row->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(i));
        row->setToolTip(0, text(entry.dir));
        row->setCheckState(0, entry.mounted ? Qt::Checked : Qt::Unchecked);
        if (list.file.empty() || list.source == assets::AddonSettings::List::Apex)
            row->setFlags(row->flags() & ~Qt::ItemIsUserCheckable);
        if (!entry.thumbnail.empty()) {
            QImageReader reader(text(entry.thumbnail.string()));
            reader.setScaledSize(QSize(64, 64));
            row->setIcon(0, QPixmap::fromImage(reader.read()));
        }
    }
    filter();
}
void AddonsPanel::filter() {
    for (int i = 0; i < entries_->topLevelItemCount(); ++i) {
        auto* row = entries_->topLevelItem(i);
        QString all = row->toolTip(0);
        for (int column = 0; column < entries_->columnCount(); ++column) all += '\n' + row->text(column);
        row->setHidden(!all.contains(filter_->text(), Qt::CaseInsensitive));
    }
}
} // namespace apex::ui
