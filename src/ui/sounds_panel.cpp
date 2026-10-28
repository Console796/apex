#include "ui/sounds_panel.hpp"

#include <QComboBox>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include "ui/game_content.hpp"

namespace apex::ui {
SoundsPanel::SoundsPanel(QWidget* parent, SoundPreview::Output output)
    : AssetBrowserPanel(Kind::Sound, QStringLiteral("sounds"), parent), output_(output) {
    type_ = new QComboBox(this);
    type_->setObjectName(QStringLiteral("soundsTypeFilter"));
    type_->addItems({tr("All sounds"), tr("Soundscript"), tr("Raw files")});
    filters_->addWidget(type_);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("soundsList"));
    tree_->setHeaderLabels({tr("Name"), tr("Source")});
    layout_->addWidget(tree_, 1);
    setPickView(tree_, [this] { play(); });
    connect(this, &AssetBrowserPanel::assetChosen, this, &SoundsPanel::soundChosen);
    // Probe the backend without opening an audio device just to browse names. The actual
    // audio output is created on the first Play; tests can retain the device-free decoder.
    preview_ = new SoundPreview(this, output == SoundPreview::Output::AudioDevice ? SoundPreview::Output::DecodeOnly : output);
    auto* buttons = new QHBoxLayout;
    play_ = new QPushButton(tr("Play"), this);
    play_->setObjectName(QStringLiteral("soundsPlay"));
    stop_ = new QPushButton(tr("Stop"), this);
    stop_->setObjectName(QStringLiteral("soundsStop"));
    next_ = new QPushButton(tr("Next wave"), this);
    next_->setObjectName(QStringLiteral("soundsNextWave"));
    buttons->addWidget(play_); buttons->addWidget(stop_); buttons->addWidget(next_); buttons->addStretch();
    layout_->addLayout(buttons);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("soundsStatus"));
    status_->setWordWrap(true);
    layout_->addWidget(status_);
    connect(type_, &QComboBox::currentIndexChanged, this, [this] { invalidate(); });
    connect(play_, &QPushButton::clicked, this, &SoundsPanel::play);
    connect(stop_, &QPushButton::clicked, this, [this] { preview_->stop(); });
    connect(next_, &QPushButton::clicked, this, [this] { preview_->next(); });
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) { remember(item->text(0)); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { script_.clear(); updatePlayback(); });
    connectPreview();
    updatePlayback();
}
void SoundsPanel::connectPreview() {
    connect(preview_, &SoundPreview::availabilityChanged, this, &SoundsPanel::updatePlayback);
    connect(preview_, &SoundPreview::playingChanged, this, &SoundsPanel::updatePlayback);
    connect(preview_, &SoundPreview::currentPathChanged, this, [this](const QString& path) { status_->setText(path); });
    connect(preview_, &SoundPreview::errorOccurred, status_, &QLabel::setText);
    updatePlayback();
}
void SoundsPanel::contentReset() {
    preview_->setVfs(content_ ? content_->vfs() : nullptr);
    script_.clear(); updatePlayback();
}
void SoundsPanel::rebuild() {
    const auto selected = tree_->currentItem() ? tree_->currentItem()->text(0) : QString{};
    tree_->clear();
    const auto snapshot = content_ ? content_->soundSnapshot() : nullptr;
    if (!snapshot) return;
    updateSources(*snapshot);
    assets::SoundFilter query;
    static_cast<assets::AssetFilter&>(query) = filter();
    if (type_->currentIndex() == 1) query.type = assets::SoundIndexEntry::Type::SoundScript;
    else if (type_->currentIndex() == 2) query.type = assets::SoundIndexEntry::Type::Raw;
    for (const auto row : assets::filterSounds(*snapshot, query)) {
        const auto& entry = snapshot->entries[row];
        const auto name = QString::fromStdString(entry.name);
        names_ << name;
        auto* item = new QTreeWidgetItem(tree_, {name, sourceName(entry.mount)});
        item->setToolTip(0, QString::fromStdString(entry.script) + "\n" + QString::fromStdString(entry.error));
        if (entry.type == assets::SoundIndexEntry::Type::SoundScript) {
            QStringList waves;
            for (const auto& wave : entry.waves) {
                const auto path = QString::fromStdString(wave);
                waves << path;
                auto* child = new QTreeWidgetItem(item, {path});
                child->setData(0, Qt::UserRole, path);
            }
            item->setData(0, Qt::UserRole + 1, waves);
        } else item->setData(0, Qt::UserRole, name);
        if (name == selected) tree_->setCurrentItem(item);
    }
    tree_->resizeColumnToContents(0);
    setCount(snapshot->entries.size()); updatePlayback();
}
void SoundsPanel::play() {
    const auto* item = tree_->currentItem();
    if (!item || !preview_->available()) return;
    if (output_ == SoundPreview::Output::AudioDevice && !audioOutputReady_) {
        delete preview_;
        preview_ = new SoundPreview(this, output_);
        preview_->setVfs(content_ ? content_->vfs() : nullptr);
        audioOutputReady_ = true;
        connectPreview();
        if (!preview_->available()) { updatePlayback(); return; }
    }
    remember(item->text(0));
    status_->clear();
    const auto waves = item->data(0, Qt::UserRole + 1).toStringList();
    if (!waves.empty()) {
        if (script_ == item->text(0)) preview_->next();
        else { script_ = item->text(0); preview_->playScript(waves); }
    } else { script_.clear(); preview_->play(item->data(0, Qt::UserRole).toString()); }
    updatePlayback();
}
void SoundsPanel::updatePlayback() {
    const auto* item = tree_->currentItem();
    const bool available = preview_->available();
    const bool waves = item && !item->data(0, Qt::UserRole + 1).toStringList().empty();
    play_->setEnabled(available && item && (waves || !item->data(0, Qt::UserRole).toString().isEmpty()));
    stop_->setEnabled(available && preview_->isPlaying());
    next_->setEnabled(available && waves && !script_.isEmpty());
    if (!available) status_->setText(tr("Sound preview unavailable: %1").arg(preview_->unavailableReason()));
}
void SoundsPanel::recentActivated(const QString& name) {
    const auto found = tree_->findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
    if (!found.empty()) { tree_->setCurrentItem(found.front()); tree_->scrollToItem(found.front()); }
}
QString SoundsPanel::pickedValue(const QString& name) const {
    // Source sound keys are relative to sound/; script names retain their spelling.
    const auto* item = tree_->currentItem();
    return item && !item->data(0, Qt::UserRole).toString().isEmpty() && name.startsWith("sound/")
        ? name.mid(6) : name;
}
void SoundsPanel::hideEvent(QHideEvent* event) {
    preview_->stop(); script_.clear(); AssetBrowserPanel::hideEvent(event);
}
} // namespace apex::ui
