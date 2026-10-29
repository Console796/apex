#include "ui/asset_browser_panel.hpp"

#include <QCheckBox>
#include <QAbstractItemView>
#include <QKeyEvent>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include "assets/model_index.hpp"
#include "assets/sound_index.hpp"
#include "assets/particle_index.hpp"
#include "core/strings.hpp"
#include "ui/game_content.hpp"

namespace apex::ui {
namespace { constexpr int kAnySource = -2, kGameSource = -1; }
AssetBrowserPanel::AssetBrowserPanel(Kind kind, QString prefix, QWidget* parent)
    : QWidget(parent), kind_(kind), prefix_(std::move(prefix)) {
    setObjectName(prefix_ + "Panel");
    layout_ = new QVBoxLayout(this);
    pickBanner_ = new QLabel(this);
    pickBanner_->setObjectName(prefix_ + "PickBanner");
    pickBanner_->setStyleSheet(QStringLiteral("QLabel { background: palette(highlight); color: palette(highlighted-text); padding: 4px; }"));
    pickBanner_->hide();
    layout_->addWidget(pickBanner_);
    choose_ = new QPushButton(tr("Choose"), this);
    choose_->setObjectName(prefix_ + "Choose");
    choose_->hide();
    layout_->addWidget(choose_);
    connect(choose_, &QPushButton::clicked, this, &AssetBrowserPanel::chooseCurrent);
    filters_ = new QHBoxLayout;
    nameFilter_ = new QLineEdit(this);
    nameFilter_->setObjectName(prefix_ + "NameFilter");
    nameFilter_->setPlaceholderText(tr("Filter names… (substring or glob)"));
    sourceFilter_ = new QComboBox(this);
    sourceFilter_->setObjectName(prefix_ + "SourceFilter");
    sourceFilter_->addItem(tr("Any source"), kAnySource);
    usedFilter_ = new QCheckBox(tr("Used in this map"), this);
    usedFilter_->setObjectName(prefix_ + "UsedFilter");
    filters_->addWidget(nameFilter_, 1); filters_->addWidget(sourceFilter_); filters_->addWidget(usedFilter_);
    layout_->addLayout(filters_);
    progress_ = new QLabel(tr("No game profile"), this);
    progress_->setObjectName(prefix_ + "Progress");
    layout_->addWidget(progress_);
    recent_ = new QListWidget(this);
    recent_->setObjectName(prefix_ + "Recent");
    recent_->setMaximumHeight(80);
    recent_->setToolTip(tr("Recently browsed"));
    recent_->addItems(QSettings().value(prefix_ + "/recent").toStringList());
    layout_->addWidget(recent_);
    connect(nameFilter_, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(sourceFilter_, &QComboBox::currentIndexChanged, this, [this] { invalidate(); });
    connect(usedFilter_, &QCheckBox::toggled, this, [this] { invalidate(); });
    connect(recent_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const auto name = item->text();
        if (pickMode_) { initial_ = name; nameFilter_->setText(name); refresh(); }
        recentActivated(name);
    });
}
AssetBrowserPanel::~AssetBrowserPanel() {
    if (document_) document_->unobserve(observer_);
}
void AssetBrowserPanel::setPickView(QAbstractItemView* view, std::function<void()> activate) {
    pickView_ = view;
    connect(view, &QAbstractItemView::doubleClicked, this, [this, activate = std::move(activate)] {
        if (pickMode_) chooseCurrent();
        else if (activate) activate();
    });
}
void AssetBrowserPanel::setPickMode(bool enabled, const QString& initial, const QString& what) {
    if (enabled && !pickMode_) filterBeforePick_ = nameFilter_->text();
    const auto previous = enabled ? initial : filterBeforePick_;
    if (!enabled) filterBeforePick_.clear();
    pickMode_ = enabled;
    initial_ = initial;
    pickBanner_->setText(tr("Choose %1; Esc cancels").arg(what));
    pickBanner_->setVisible(enabled);
    choose_->setVisible(enabled);
    installEventFilter(this);
    for (auto* widget : findChildren<QWidget*>()) widget->installEventFilter(this);
    pickModeChanged(enabled);
    nameFilter_->setText(previous);
    invalidate();
    refresh();
    if (enabled) nameFilter_->setFocus(Qt::OtherFocusReason);
}
bool AssetBrowserPanel::eventFilter(QObject* object, QEvent* event) {
    if (pickMode_ && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Escape) { emit pickCancelled(); return true; }
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && (object == pickView_ || object == nameFilter_)) {
            chooseCurrent(); return true;
        }
    }
    return QWidget::eventFilter(object, event);
}
void AssetBrowserPanel::selectName(const QString& name) {
    if (!pickView_) return;
    const auto key = [this](const QString& value) {
        return kind_ == Kind::Sound ? assets::SoundKind::key(value.toStdString()) : toLowerAscii(value.toStdString());
    };
    const auto visit = [&](auto&& self, const QModelIndex& parent) -> bool {
        for (int row = 0; row < pickView_->model()->rowCount(parent); ++row) {
            const auto index = pickView_->model()->index(row, 0, parent);
            if (key(index.data().toString()) == key(name)) {
                pickView_->setCurrentIndex(index); pickView_->scrollTo(index); return true;
            }
            if (self(self, index)) return true;
        }
        return false;
    };
    visit(visit, {});
}
void AssetBrowserPanel::chooseCurrent() {
    if (!pickMode_ || !pickView_ || !pickView_->currentIndex().isValid()) return;
    const auto name = pickView_->currentIndex().siblingAtColumn(0).data().toString();
    if (name.isEmpty()) return;
    remember(name);
    emit assetChosen(pickedValue(name));
}
void AssetBrowserPanel::setGameContent(GameContent* content) {
    if (content_) disconnect(content_, nullptr, this, nullptr);
    content_ = content;
    contentReset();
    if (content_) {
        connect(content_, &GameContent::contentChanged, this, [this] {
            contentReset(); usedDirty_ = true; invalidate();
        });
        connect(content_, &GameContent::classesReady, this, [this] { usedDirty_ = true; invalidate(); });
        const auto ready = [this] { invalidate(); };
        const auto progress = [this](std::size_t done, std::size_t total) { if (isVisible()) updateProgress(done, total); };
        switch (kind_) {
        case Kind::Model:
            connect(content_, &GameContent::modelIndexReady, this, ready);
            connect(content_, &GameContent::modelIndexProgress, this, progress); break;
        case Kind::Sound:
            connect(content_, &GameContent::soundIndexReady, this, ready);
            connect(content_, &GameContent::soundIndexProgress, this, progress); break;
        case Kind::Particle:
            connect(content_, &GameContent::particleIndexReady, this, ready);
            connect(content_, &GameContent::particleIndexProgress, this, progress); break;
        }
    }
    usedDirty_ = true; invalidate();
}
void AssetBrowserPanel::setDocument(document::Document* document) {
    if (document_) document_->unobserve(observer_);
    document_ = document;
    if (document_) observer_ = document_->observe([this](const document::ChangeSet&) { usedDirty_ = true; invalidate(); });
    usedDirty_ = true; invalidate();
}
void AssetBrowserPanel::invalidate() { dirty_ = true; if (isVisible()) refresh(); }
void AssetBrowserPanel::showEvent(QShowEvent* event) { QWidget::showEvent(event); refresh(); }
QStringList AssetBrowserPanel::filteredNames() { refresh(); return names_; }
void AssetBrowserPanel::refresh() {
    if (!dirty_) return;
    dirty_ = false;
    if (usedDirty_) { collectUsed(); usedDirty_ = false; }
    names_.clear();
    updateProgress(0, 0);
    rebuild();
    if (pickMode_ && !initial_.isEmpty()) selectName(initial_);
}
void AssetBrowserPanel::updateProgress(std::size_t done, std::size_t total) {
    if (!content_ || !content_->hasProfile()) progress_->setText(tr("No game profile"));
    else if (!content_->error().isEmpty()) progress_->setText(content_->error());
    else progress_->setText(tr("Indexing… %1 of %2").arg(done).arg(total));
}
void AssetBrowserPanel::setCount(std::size_t count) { progress_->setText(tr("%1 entries").arg(count)); }
QString AssetBrowserPanel::sourceName(std::optional<std::size_t> mount) const {
    if (!mount) return tr("Game content");
    const auto name = content_ ? content_->addonName(*mount) : QString{};
    return name.isEmpty() ? tr("Addon %1").arg(*mount) : name;
}
void AssetBrowserPanel::updateSources(const std::set<std::optional<std::size_t>>& sources) {
    const QSignalBlocker block(sourceFilter_);
    const auto selected = sourceFilter_->currentData();
    sourceFilter_->clear(); sourceFilter_->addItem(tr("Any source"), kAnySource);
    for (const auto source : sources)
        sourceFilter_->addItem(sourceName(source), source ? static_cast<int>(*source) : kGameSource);
    sourceFilter_->setCurrentIndex(std::max(0, sourceFilter_->findData(selected)));
}
assets::AssetFilter AssetBrowserPanel::filter() const {
    assets::AssetFilter filter;
    filter.name = nameFilter_->text().toStdString();
    const auto source = sourceFilter_->currentData().toInt();
    if (source == kGameSource) filter.source.emplace();
    else if (source >= 0) filter.source.emplace(static_cast<std::size_t>(source));
    if (usedFilter_->isChecked()) filter.usedInMap = used_;
    return filter;
}
void AssetBrowserPanel::remember(const QString& name) {
    auto names = QSettings().value(prefix_ + "/recent").toStringList();
    names.removeAll(name); names.prepend(name);
    while (names.size() > 24) names.removeLast();
    QSettings().setValue(prefix_ + "/recent", names);
    recent_->clear(); recent_->addItems(names);
}
void AssetBrowserPanel::collectUsed() {
    used_.clear();
    if (!document_) return;
    const auto* classes = content_ ? content_->classes() : nullptr;
    const auto visit = [&](auto&& self, document::NodeId id) -> void {
        const auto handle = document_->handle(id);
        const auto* node = handle ? document_->get(*handle) : nullptr;
        if (!node) return;
        if (node->block && ciEquals(node->block->key(), "entity")) {
            const auto* classname = node->block->find("classname");
            const auto* cls = classes && classname ? classes->find(classname->decodedValue().value_or("")) : nullptr;
            for (const auto& key : node->block->children()) {
                const auto value = key.decodedValue();
                if (!value) continue;
                std::string type;
                if (cls) for (const auto& property : cls->properties)
                    if (ciEquals(property.name, key.key())) { type = property.type; break; }
                if (kind_ == Kind::Model && (type == "studio" || ciEquals(key.key(), "model")))
                    used_.insert(assets::ModelKind::key(*value));
                else if (kind_ == Kind::Sound && (type == "sound" || ciEquals(key.key(), "message") || ciEquals(key.key(), "soundname")))
                    used_.insert(assets::SoundKind::key(*value));
                else if (kind_ == Kind::Particle && type == "particlesystem")
                    used_.insert(assets::ParticleKind::key(*value));
            }
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : document_->roots()) visit(visit, root);
}
} // namespace apex::ui
