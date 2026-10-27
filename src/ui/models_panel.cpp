#include "ui/models_panel.hpp"

#include <QAbstractListModel>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QImage>
#include <QListView>
#include <QMimeData>
#include <QPainter>
#include <QSettings>
#include <QItemSelectionModel>
#include <QSlider>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include "render/model_inspector.hpp"
#include "ui/game_content.hpp"
#include "ui/model_inspect_view.hpp"

namespace apex::ui {
class ModelsPanel::GridModel final : public QAbstractListModel {
public:
    explicit GridModel(ModelsPanel& panel) : QAbstractListModel(&panel), panel_(panel) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        return index.isValid() ? QAbstractListModel::flags(index) | Qt::ItemIsDragEnabled : Qt::NoItemFlags;
    }
    QStringList mimeTypes() const override { return {QStringLiteral("application/x-apex-model"), QStringLiteral("text/plain")}; }
    QMimeData* mimeData(const QModelIndexList& indexes) const override {
        auto* mime = new QMimeData;
        if (!indexes.empty()) {
            const auto name = indexes.front().data().toString();
            mime->setData(QStringLiteral("application/x-apex-model"), name.toUtf8());
            mime->setText(name);
        }
        return mime;
    }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= rows_.size() || !snapshot_) return {};
        const auto& entry = snapshot_->entries[rows_[static_cast<std::size_t>(index.row())]];
        if (role == Qt::DisplayRole) return QString::fromStdString(entry.name);
        if (role == Qt::ToolTipRole)
            return QString::fromStdString(entry.name) + tr("\n%1 body parts; %2 skin families, %3 skins per family\n%4")
                .arg(entry.bodyPartCount).arg(entry.skinFamilyCount).arg(entry.skinsPerFamily)
                .arg(panel_.sourceName(entry.mount)) + (entry.error.empty() ? QString{} : "\n" + QString::fromStdString(entry.error));
        if (role != Qt::DecorationRole) return {};
        const int size = panel_.grid_->iconSize().width();
        QPixmap pixmap(size, size);
        pixmap.fill(QColor(96, 99, 104));
        auto* cache = panel_.content_ ? panel_.content_->thumbnails() : nullptr;
        auto image = cache ? cache->request(assets::ThumbnailKind::Model, entry.name, 128) : nullptr;
        if (image) {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(pixmap.rect(), QImage(reinterpret_cast<const uchar*>(image->rgba.data()), image->width,
                image->height, image->width * 4, QImage::Format_RGBA8888));
        }
        return pixmap;
    }
    void setRows(std::shared_ptr<const assets::ModelIndexSnapshot> snapshot, std::vector<std::size_t> rows) {
        if (snapshot == snapshot_ && rows == rows_) return;
        beginResetModel(); snapshot_ = std::move(snapshot); rows_ = std::move(rows); endResetModel();
    }
    void touch() { if (rowCount()) emit dataChanged(index(0), index(rowCount() - 1), {Qt::DecorationRole}); }
private:
    ModelsPanel& panel_;
    std::shared_ptr<const assets::ModelIndexSnapshot> snapshot_;
    std::vector<std::size_t> rows_;
};
ModelsPanel::ModelsPanel(QWidget* parent) : AssetBrowserPanel(Kind::Model, QStringLiteral("models"), parent) {
    staticOnly_ = new QCheckBox(tr("Static props only"), this);
    staticOnly_->setObjectName(QStringLiteral("modelsStaticFilter"));
    filters_->addWidget(staticOnly_);
    grid_ = new QListView(this);
    grid_->setObjectName(QStringLiteral("modelsGrid"));
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setUniformItemSizes(true);
    grid_->setWordWrap(false);
    grid_->setTextElideMode(Qt::ElideMiddle);
    grid_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    grid_->setDragEnabled(true);
    model_ = new GridModel(*this);
    grid_->setModel(model_);
    split_ = new QSplitter(Qt::Vertical, this);
    split_->setObjectName(QStringLiteral("modelsSplit"));
    split_->setChildrenCollapsible(false);
    inspect_ = new ModelInspectView(this);
    inspect_->setMinimumHeight(140);
    inspect_->clearModel();
    split_->addWidget(grid_);
    split_->addWidget(inspect_);
    split_->setStretchFactor(0, 1);
    layout_->addWidget(split_, 1);
    setPickView(grid_);
    connect(this, &AssetBrowserPanel::assetChosen, this, &ModelsPanel::modelChosen);
    auto* row = new QHBoxLayout;
    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("modelsViewMode"));
    mode_->addItems({tr("Grid"), tr("List")});
    mode_->setCurrentIndex(QSettings().value("models/listMode", false).toBool() ? 1 : 0);
    size_ = new QSlider(Qt::Horizontal, this);
    size_->setObjectName(QStringLiteral("modelsThumbnailSize"));
    size_->setToolTip(tr("Thumbnail size"));
    size_->setRange(32, 256);
    size_->setValue(std::clamp(QSettings().value("models/thumbnailSize", 64).toInt(), 32, 256));
    inspectToggle_ = new QCheckBox(tr("Inspect"), this);
    inspectToggle_->setObjectName(QStringLiteral("modelsInspectToggle"));
    inspectToggle_->setToolTip(tr("Show a larger, turnable view of the selected model"));
    inspectToggle_->setChecked(QSettings().value("models/inspect", true).toBool());
    inspect_->setVisible(inspectToggle_->isChecked());
    const auto saved = QSettings().value("models/inspectSplit").toList();
    split_->setSizes(saved.size() == 2 ? QList<int>{saved[0].toInt(), saved[1].toInt()} : QList<int>{300, 220});
    row->addWidget(mode_); row->addWidget(size_, 1); row->addWidget(inspectToggle_); layout_->addLayout(row);
    connect(inspectToggle_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("models/inspect", on);
        inspect_->setVisible(on);
        if (!on) releaseInspector();
    });
    connect(split_, &QSplitter::splitterMoved, this, [this] {
        if (inspect_->isVisible()) QSettings().setValue("models/inspectSplit", QVariantList{split_->sizes()[0], split_->sizes()[1]});
    });
    connect(grid_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& index) {
        if (index.isValid()) inspect(index.data().toString());
    });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        QSettings().setValue("models/listMode", mode_->currentIndex() == 1); updateGridSize();
    });
    connect(size_, &QSlider::valueChanged, this, [this](int value) {
        QSettings().setValue("models/thumbnailSize", value); updateGridSize(); model_->touch();
    });
    connect(staticOnly_, &QCheckBox::toggled, this, [this] { invalidate(); });
    connect(grid_, &QListView::clicked, this, [this](const QModelIndex& index) { recentActivated(index.data().toString()); });
    updateGridSize();
}
ModelsPanel::~ModelsPanel() { releaseThumbnails(); releaseInspector(); }
void ModelsPanel::setInspectSink(InspectSink sink) { releaseInspector(); inspectSink_ = std::move(sink); }
void ModelsPanel::inspect(const QString& name) {
    const auto* snapshot = content_ ? content_->modelSnapshot().get() : nullptr;
    const auto* entry = snapshot ? snapshot->find(name.toStdString()) : nullptr;
    if (!entry) return;
    inspected_ = QString::fromStdString(entry->name);
    ModelInspectView::Summary summary;
    summary.name = inspected_;
    summary.source = sourceName(entry->mount);
    summary.staticProp = entry->staticProp;
    summary.bodyParts = static_cast<int>(entry->bodyPartCount);
    summary.skinFamilies = static_cast<int>(entry->skinFamilyCount);
    summary.skinsPerFamily = static_cast<int>(entry->skinsPerFamily);
    for (int axis = 0; axis < 3; ++axis) summary.hullSize[axis] = entry->hullMax[axis] - entry->hullMin[axis];
    inspect_->setModel(summary);
}
void ModelsPanel::releaseInspector() {
    if (inspecting_ && inspectSink_) inspectSink_({}, {}, false);
    inspecting_ = false;
}
void ModelsPanel::syncInspector(bool interacting) {
    if (!inspect_->isVisible() || inspected_.isEmpty()) { releaseInspector(); return; }
    if (!inspectSink_) { inspect_->setInspection(nullptr); return; }
    inspecting_ = true;
    inspect_->setInspection(inspectSink_(inspected_, inspect_->view(), interacting));
}
void ModelsPanel::recentActivated(const QString& name) {
    if (!content_ || !content_->modelSnapshot() || !content_->modelSnapshot()->find(name.toStdString())) return;
    remember(name);
    inspect(name);
    if (pickMode()) selectName(name);
    else emit modelActivated(name);
}
void ModelsPanel::updateGridSize() {
    const bool list = mode_->currentIndex() == 1;
    grid_->setViewMode(list ? QListView::ListMode : QListView::IconMode);
    grid_->setFlow(list ? QListView::TopToBottom : QListView::LeftToRight);
    grid_->setWrapping(!list);
    const int size = size_->value();
    grid_->setIconSize(QSize(size, size));
    const QFontMetrics metrics(grid_->font());
    grid_->setGridSize(QSize(list ? 0 : std::max(size, metrics.averageCharWidth() * 8) + 16,
                            size + metrics.lineSpacing() + 16));
}
void ModelsPanel::changeEvent(QEvent* event) {
    AssetBrowserPanel::changeEvent(event);
    if (event->type() == QEvent::FontChange && grid_ && size_ && mode_) updateGridSize();
}
void ModelsPanel::rebuild() {
    const auto snapshot = content_ ? content_->modelSnapshot() : nullptr;
    if (!snapshot) { releaseThumbnails(); model_->setRows({}, {}); return; }
    updateSources(*snapshot);
    assets::ModelFilter query;
    static_cast<assets::AssetFilter&>(query) = filter();
    query.staticPropOnly = staticOnly_->isChecked();
    auto rows = assets::filterModels(*snapshot, query);
    for (const auto row : rows) names_ << QString::fromStdString(snapshot->entries[row].name);
    model_->setRows(snapshot, std::move(rows));
    setCount(snapshot->entries.size());
}
void ModelsPanel::contentReset() {
    releaseThumbnails();
    releaseInspector();
    inspected_.clear();
    inspect_->clearModel();
    disconnect(thumbnailsConnection_);
    if (content_) thumbnailsConnection_ = connect(content_, &GameContent::thumbnailsReady, this, [this] {
        if (isVisible()) model_->touch();
    });
}
void ModelsPanel::setThumbnailSink(ThumbnailRequest request) { releaseThumbnails(); request_ = std::move(request); }
void ModelsPanel::releaseThumbnails() {
    if (request_) for (const auto& path : requested_) request_(path, false);
    requested_.clear();
}
void ModelsPanel::hideEvent(QHideEvent* event) { releaseThumbnails(); releaseInspector(); AssetBrowserPanel::hideEvent(event); }
void ModelsPanel::syncThumbnails(bool interacting) {
    if (!isVisible()) { releaseThumbnails(); releaseInspector(); return; }
    syncInspector(interacting);
    if (interacting || !request_) return;
    refresh();
    const auto viewport = grid_->viewport()->rect();
    auto anchor = grid_->indexAt(viewport.center());
    // Anchor in the viewport, then walk the contiguous visible rows. Scrollbar units and
    // item heights differ between grid/list modes, so use the view's actual item geometry.
    const int step = std::max(1, grid_->iconSize().width() / 2);
    for (int y = 0; !anchor.isValid() && y < viewport.height(); y += step)
        for (int x = 0; !anchor.isValid() && x < viewport.width(); x += step)
            anchor = grid_->indexAt(QPoint(x, y));
    std::set<QString> visible;
    if (anchor.isValid()) {
        int first = anchor.row();
        while (first > 0 && grid_->visualRect(model_->index(first - 1)).bottom() >= 0) --first;
        for (int row = first; row < model_->rowCount(); ++row) {
            const auto index = model_->index(row);
            const auto rect = grid_->visualRect(index);
            if (rect.top() >= viewport.height()) break;
            if (!rect.intersects(viewport)) continue;
            const auto path = index.data().toString();
            visible.insert(path);
            // Cache eviction can make an already visible image unloaded again.
            request_(path, true); ++requests_;
        }
    }
    for (const auto& path : requested_) if (!visible.contains(path)) request_(path, false);
    requested_ = std::move(visible);
}
} // namespace apex::ui
