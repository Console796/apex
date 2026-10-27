#include "ui/particles_panel.hpp"

#include <QTreeWidget>
#include <QVBoxLayout>
#include "ui/game_content.hpp"

namespace apex::ui {
ParticlesPanel::ParticlesPanel(QWidget* parent) : AssetBrowserPanel(Kind::Particle, QStringLiteral("particles"), parent) {
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("particlesList"));
    tree_->setHeaderLabels({tr("System"), tr("PCF"), tr("Source")});
    tree_->setRootIsDecorated(false);
    layout_->addWidget(tree_, 1);
    setPickView(tree_);
    connect(this, &AssetBrowserPanel::assetChosen, this, &ParticlesPanel::particleChosen);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) { remember(item->text(0)); });
}
void ParticlesPanel::rebuild() {
    const auto selected = tree_->currentItem() ? tree_->currentItem()->text(0) : QString{};
    tree_->clear();
    const auto snapshot = content_ ? content_->particleSnapshot() : nullptr;
    if (!snapshot) return;
    updateSources(*snapshot);
    assets::ParticleFilter query;
    static_cast<assets::AssetFilter&>(query) = filter();
    for (const auto row : assets::filterParticles(*snapshot, query)) {
        const auto& entry = snapshot->entries[row];
        const auto name = QString::fromStdString(entry.name);
        names_ << name;
        auto* item = new QTreeWidgetItem(tree_, {name, QString::fromStdString(entry.pcf), sourceName(entry.mount)});
        item->setToolTip(0, item->text(1) + "\n" + item->text(2) + "\n" + QString::fromStdString(entry.error));
        if (name == selected) tree_->setCurrentItem(item);
    }
    tree_->resizeColumnToContents(0);
    setCount(snapshot->entries.size());
}
void ParticlesPanel::recentActivated(const QString& name) {
    const auto found = tree_->findItems(name, Qt::MatchExactly);
    if (!found.empty()) { tree_->setCurrentItem(found.front()); tree_->scrollToItem(found.front()); }
}
} // namespace apex::ui
