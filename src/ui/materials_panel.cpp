#include "ui/materials_panel.hpp"

#include <QAbstractListModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDrag>
#include <QEvent>
#include <QKeyEvent>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <map>

#include "assets/material_index.hpp"
#include "assets/thumbnail_cache.hpp"
#include "core/strings.hpp"
#include "document/document.hpp"
#include "editor/editor_session.hpp"
#include "ui/game_content.hpp"

namespace apex::ui {
namespace {
QString q(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
const QString kMaterialMime = QStringLiteral("application/x-apex-material");
constexpr int kAnySource = -2, kGameSource = -1;
QStringList recentMaterials() {
    return QSettings().value(QStringLiteral("materials/recent")).toStringList();
}
void collectMaterials(const document::Document& doc, document::NodeId id, std::set<std::string>& out) {
    const auto handle = doc.handle(id);
    const auto* node = handle ? doc.get(*handle) : nullptr;
    if (!node) return;
    if (node->block) for (const auto& child : node->block->children()) {
        if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
        if (const auto* key = child.find("material"))
            if (auto value = key->decodedValue()) out.insert(toLowerAscii(*value));
    }
    for (const auto child : node->children) collectMaterials(doc, child, out);
}
class MaterialList final : public QListView {
public:
    using QListView::QListView;
    std::function<QString(const QModelIndex&)> material;
protected:
    void startDrag(Qt::DropActions) override {
        if (!material) return;
        const auto index = currentIndex();
        if (!index.isValid()) return;
        const auto name = material(index);
        if (name.isEmpty()) return;
        auto* mime = new QMimeData;
        mime->setData(kMaterialMime, name.toUtf8());
        mime->setText(name);
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(QPixmap(48, 48));
        drag->exec(Qt::CopyAction);
    }
};
}

class MaterialsPanel::GridModel final : public QAbstractListModel {
public:
    explicit GridModel(MaterialsPanel& panel) : QAbstractListModel(&panel), panel_(panel) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        return index.isValid() ? QAbstractListModel::flags(index) | Qt::ItemIsDragEnabled : Qt::NoItemFlags;
    }
    QVariant data(const QModelIndex& modelIndex, int role) const override {
        if (!modelIndex.isValid() || modelIndex.row() < 0 || static_cast<std::size_t>(modelIndex.row()) >= rows_.size()) return {};
        const auto* snapshot = panel_.content_ ? panel_.content_->snapshot().get() : nullptr;
        if (!snapshot) return {};
        const auto& entry = snapshot->entries[rows_[static_cast<std::size_t>(modelIndex.row())]];
        if (role == Qt::DisplayRole) return q(entry.name);
        if (role == Qt::ToolTipRole) {
            QString tip = q(entry.name) + QStringLiteral("\n") + q(entry.shader);
            if (entry.mount) {
                const auto addon = panel_.content_->addonName(*entry.mount);
                tip += QStringLiteral("\n") + (addon.isEmpty() ? QObject::tr("Addon %1").arg(*entry.mount) : addon);
            }
            if (!entry.error.empty()) tip += QStringLiteral("\n") + q(entry.error);
            return tip;
        }
        if (role == Qt::FontRole && panel_.session_ && ciEquals(entry.name, panel_.session_->newSolidStyle().material)) {
            QFont font; font.setBold(true); return font;
        }
        if (role == Qt::DecorationRole) {
            auto* cache = panel_.content_->thumbnails();
            if (!cache) return framed({});
            auto thumb = cache->request(entry.name, panel_.size_->value());
            ++requests_;
            if (!thumb) return framed({});
            return framed(QImage(reinterpret_cast<const uchar*>(thumb->rgba.data()), thumb->width, thumb->height,
                                 thumb->width * 4, QImage::Format_RGBA8888));
        }
        return {};
    }
    // A reset invalidates the view's pressed index, so a click that changes the active material
    // would swallow the double-click that follows. Unchanged rows only repaint.
    void setRows(std::vector<std::size_t> rows) {
        if (rows == rows_) { touch({Qt::DisplayRole, Qt::FontRole, Qt::ToolTipRole}); return; }
        beginResetModel(); rows_ = std::move(rows); endResetModel();
    }
    void touch(const QList<int>& roles) {
        if (!rows_.empty()) emit dataChanged(index(0), index(static_cast<int>(rows_.size()) - 1), roles);
    }
    const std::vector<std::size_t>& rows() const { return rows_; }
    std::size_t requests() const { return requests_; }
    // Every decoration is exactly the icon size: the view uses uniform item sizes measured from one
    // item, so a placeholder of another size set every cell's height and cut the names off. A null
    // image draws the placeholder. Drawn at the screen's pixel ratio so scaled displays stay sharp.
    QPixmap framed(const QImage& image) const {
        const int size = panel_.size_->value();
        const qreal ratio = panel_.grid_->devicePixelRatioF();
        QPixmap pixmap(QSize(size, size) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        if (image.isNull()) {
            painter.fillRect(QRect(0, 0, size, size), QColor(96, 99, 104));
            painter.setPen(QColor(120, 123, 128));
            painter.drawRect(0, 0, size - 1, size - 1);
            return pixmap;
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSizeF fit = QSizeF(image.size()).scaled(size, size, Qt::KeepAspectRatio);
        painter.drawImage(QRectF(QPointF((size - fit.width()) / 2, (size - fit.height()) / 2), fit), image);
        return pixmap;
    }
private:
    MaterialsPanel& panel_;
    std::vector<std::size_t> rows_;
    mutable std::size_t requests_ = 0;
};

MaterialsPanel::MaterialsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("materialsPanel"));
    auto* layout = new QVBoxLayout(this);
    pickBanner_ = new QLabel(this);
    pickBanner_->setObjectName(QStringLiteral("materialsPickBanner"));
    pickBanner_->setStyleSheet(QStringLiteral("QLabel { background: palette(highlight); color: palette(highlighted-text); padding: 4px; }"));
    pickBanner_->hide();
    layout->addWidget(pickBanner_);
    auto* filters = new QHBoxLayout;
    nameFilter_ = new QLineEdit(this);
    nameFilter_->setObjectName(QStringLiteral("materialsNameFilter"));
    nameFilter_->setPlaceholderText(tr("Filter materials… (keyword:word)"));
    shaderFilter_ = new QComboBox(this);
    shaderFilter_->setObjectName(QStringLiteral("materialsShaderFilter"));
    shaderFilter_->addItem(tr("Any shader"), QString{});
    sourceFilter_ = new QComboBox(this);
    sourceFilter_->setObjectName(QStringLiteral("materialsSourceFilter"));
    sourceFilter_->setToolTip(tr("Where materials come from: the game, or one mounted addon"));
    sourceFilter_->addItem(tr("Any source"), kAnySource);
    usedFilter_ = new QCheckBox(tr("Used in this map"), this);
    usedFilter_->setObjectName(QStringLiteral("materialsUsedFilter"));
    toolFilter_ = new QCheckBox(tr("Tool textures"), this);
    toolFilter_->setObjectName(QStringLiteral("materialsToolFilter"));
    toolFilter_->setChecked(true);
    filters->addWidget(nameFilter_, 2); filters->addWidget(shaderFilter_); filters->addWidget(sourceFilter_); filters->addWidget(usedFilter_); filters->addWidget(toolFilter_);
    layout->addLayout(filters);
    progress_ = new QLabel(tr("No game profile"), this);
    progress_->setObjectName(QStringLiteral("materialsProgress"));
    layout->addWidget(progress_);
    auto* body = new QHBoxLayout;
    recent_ = new QListWidget(this);
    recent_->setObjectName(QStringLiteral("materialsRecent"));
    recent_->setMaximumWidth(170);
    recent_->addItems(recentMaterials());
    body->addWidget(recent_);
    grid_ = new MaterialList(this);
    grid_->setObjectName(QStringLiteral("materialsGrid"));
    grid_->setViewMode(QListView::IconMode);
    grid_->setFlow(QListView::LeftToRight);
    grid_->setWrapping(true);
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    grid_->setUniformItemSizes(true);
    grid_->setDragEnabled(true);
    // One line per name, elided in the middle so the folder and the leaf both show; the tooltip has
    // the whole name. Icon mode wraps by default, which outgrew a fixed cell height.
    grid_->setWordWrap(false);
    grid_->setTextElideMode(Qt::ElideMiddle);
    model_ = new GridModel(*this);
    grid_->setModel(model_);
    static_cast<MaterialList*>(grid_)->material = [this](const QModelIndex& index) { return index.data(Qt::DisplayRole).toString(); };
    body->addWidget(grid_, 1);
    layout->addLayout(body, 1);
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(new QLabel(tr("Thumbnail size"), this));
    size_ = new QSlider(Qt::Horizontal, this);
    size_->setObjectName(QStringLiteral("materialsThumbnailSize"));
    size_->setRange(32, 256);
    size_->setValue(std::clamp(QSettings().value(QStringLiteral("materials/thumbnailSize"), 64).toInt(), 32, 256));
    sizeRow->addWidget(size_, 1);
    layout->addLayout(sizeRow);
    updateGridSize();
    connect(nameFilter_, &QLineEdit::textChanged, this, [this] { refilter(); });
    connect(shaderFilter_, &QComboBox::currentIndexChanged, this, [this] { refilter(); });
    connect(sourceFilter_, &QComboBox::currentIndexChanged, this, [this] { refilter(); });
    connect(usedFilter_, &QCheckBox::toggled, this, [this] { refilter(); });
    connect(toolFilter_, &QCheckBox::toggled, this, [this] { refilter(); });
    connect(size_, &QSlider::valueChanged, this, [this](int value) {
        QSettings().setValue(QStringLiteral("materials/thumbnailSize"), value);
        updateGridSize();
        model_->touch({Qt::DecorationRole});
    });
    connect(grid_, &QListView::clicked, this, [this](const QModelIndex& index) { activate(index.data().toString()); });
    connect(grid_, &QListView::doubleClicked, this, [this](const QModelIndex& index) { applyToSelection(index.data().toString()); });
    connect(recent_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { activate(item->text()); });
    setPickMode(false);
    installEventFilter(this);
    for (auto* child : findChildren<QWidget*>()) child->installEventFilter(this);
}
MaterialsPanel::~MaterialsPanel() { setDocument(nullptr); setEditorSession(nullptr); }
void MaterialsPanel::setGameContent(GameContent* content) {
    if (content_ == content) return;
    if (content_) disconnect(content_, nullptr, this, nullptr);
    content_ = content;
    if (content_) {
        connect(content_, &GameContent::contentChanged, this, [this] { updateProgress(); refilter(); });
        connect(content_, &GameContent::indexProgress, this, [this](std::size_t done, std::size_t total) { done_ = done; total_ = total; updateProgress(); });
        connect(content_, &GameContent::indexReady, this, [this] {
            updateProgress(); updateShaders(); updateSources(); refilter();
        });
        connect(content_, &GameContent::thumbnailsReady, this, [this] {
            model_->touch({Qt::DecorationRole});
        });
    }
    updateProgress(); updateShaders(); updateSources(); refilter();
}
void MaterialsPanel::updateShaders() {
    if (!content_ || !content_->snapshot()) return;
    const QSignalBlocker block(shaderFilter_);
    const auto selected = shaderFilter_->currentData().toString();
    shaderFilter_->clear(); shaderFilter_->addItem(tr("Any shader"), QString{});
    std::set<std::string> shaders;
    for (const auto& entry : content_->snapshot()->entries) if (!entry.shader.empty()) shaders.insert(entry.shader);
    for (const auto& value : shaders) shaderFilter_->addItem(q(value), q(value));
    shaderFilter_->setCurrentIndex(std::max(0, shaderFilter_->findData(selected)));
}
void MaterialsPanel::updateGridSize() {
    const int icon = size_->value();
    const QFontMetrics metrics(grid_->font());
    grid_->setIconSize(QSize(icon, icon));
    // Icon, one text line and the delegate's margins, from the font so display scaling fits.
    grid_->setGridSize(QSize(std::max(icon, metrics.averageCharWidth() * 8) + 16, icon + metrics.lineSpacing() + 16));
}
void MaterialsPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange && grid_ && size_) updateGridSize();
}
void MaterialsPanel::updateSources() {
    const QSignalBlocker block(sourceFilter_);
    const auto selected = sourceFilter_->currentData().toInt();
    sourceFilter_->clear();
    sourceFilter_->addItem(tr("Any source"), kAnySource);
    const auto snapshot = content_ ? content_->snapshot() : nullptr;
    if (snapshot) {
        // Only sources that serve at least one material, in mount-list order.
        std::size_t game = 0;
        std::map<std::size_t, std::size_t> addons;
        for (const auto& entry : snapshot->entries) entry.mount ? ++addons[*entry.mount] : ++game;
        if (game) sourceFilter_->addItem(tr("Game content (%1)").arg(game), kGameSource);
        for (const auto& [addon, count] : addons) {
            auto name = content_->addonName(addon);
            if (name.isEmpty()) name = tr("Addon %1").arg(addon);
            sourceFilter_->addItem(tr("%1 (%2)").arg(name).arg(count), static_cast<int>(addon));
        }
    }
    sourceFilter_->setCurrentIndex(std::max(0, sourceFilter_->findData(selected)));
}
void MaterialsPanel::setDocument(document::Document* document) {
    if (document_) document_->unobserve(documentObserver_);
    document_ = document;
    if (document_) documentObserver_ = document_->observe([this](const document::ChangeSet&) { updateUsedMaterials(); refilter(); });
    updateUsedMaterials(); refilter();
}
void MaterialsPanel::setEditorSession(editor::EditorSession* session) {
    if (session_) session_->unobserve(sessionObserver_);
    session_ = session;
    // Sessions notify on every hover; only an active-material change matters here.
    if (session_) sessionObserver_ = session_->observe([this] {
        if (session_->newSolidStyle().material == active_) return;
        active_ = session_->newSolidStyle().material;
        model_->touch({Qt::FontRole});
    });
    active_ = session_ ? session_->newSolidStyle().material : std::string{};
    refilter();
}
void MaterialsPanel::setPickMode(bool enabled, const QString& initial, const QString& what) {
    // An area panel picks once and then shows what the user had filtered before.
    if (enabled && !pickMode_) filterBeforePick_ = nameFilter_->text();
    const QString filter = enabled ? initial : filterBeforePick_;
    if (!enabled) filterBeforePick_.clear();
    pickMode_ = enabled;
    pickMaterial_ = initial;
    pickBanner_->setText(tr("Choose a material for %1; Esc cancels").arg(what));
    pickBanner_->setVisible(enabled);
    nameFilter_->setText(filter);
    recent_->setVisible(!enabled);
    if (enabled) nameFilter_->setFocus(Qt::OtherFocusReason);
}
bool MaterialsPanel::eventFilter(QObject* object, QEvent* event) {
    if (pickMode_ && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        emit pickCancelled();
        return true;
    }
    return QWidget::eventFilter(object, event);
}
void MaterialsPanel::updateProgress() {
    if (!content_ || !content_->hasProfile()) { progress_->setText(tr("No game profile")); return; }
    if (!content_->error().isEmpty()) { progress_->setText(tr("Game content unavailable: %1").arg(content_->error())); return; }
    const auto snapshot = content_->snapshot();
    if (snapshot) progress_->setText(tr("%1 materials").arg(snapshot->entries.size()));
    else progress_->setText(tr("Indexing materials… %1 of %2").arg(done_).arg(total_));
}
void MaterialsPanel::updateUsedMaterials() {
    used_.clear();
    if (document_) for (const auto root : document_->roots()) collectMaterials(*document_, root, used_);
}
void MaterialsPanel::refilter() {
    const auto snapshot = content_ ? content_->snapshot() : nullptr;
    if (!snapshot) { model_->setRows({}); return; }
    assets::MaterialFilter filter;
    filter.name = nameFilter_->text().toStdString();
    filter.shader = shaderFilter_->currentData().toString().toStdString();
    filter.toolTextures = toolFilter_->isChecked();
    if (const int source = sourceFilter_->currentData().toInt(); source == kGameSource) filter.source.emplace();
    else if (source >= 0) filter.source.emplace(static_cast<std::size_t>(source));
    if (usedFilter_->isChecked()) filter.usedInMap = used_;
    model_->setRows(assets::filterMaterials(*snapshot, filter));
    if (pickMode_ && !pickMaterial_.isEmpty()) {
        for (int row = 0; row < model_->rowCount(); ++row)
            if (ciEquals(model_->index(row).data().toString().toStdString(), pickMaterial_.toStdString())) {
                const auto index = model_->index(row);
                grid_->setCurrentIndex(index);
                grid_->scrollTo(index);
                break;
            }
    }
}
void MaterialsPanel::activate(const QString& name) {
    if (pickMode_) { remember(name); emit materialChosen(name); return; }
    if (session_) session_->setNewSolidMaterial(name.toStdString());
    remember(name);
    emit activeMaterialRequested(name);
}
void MaterialsPanel::remember(const QString& name) {
    auto values = recentMaterials();
    values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& value) {
        return value.compare(name, Qt::CaseInsensitive) == 0;
    }), values.end());
    values.prepend(name);
    while (values.size() > 16) values.removeLast();
    QSettings().setValue(QStringLiteral("materials/recent"), values);
    recent_->clear(); recent_->addItems(values);
}
void MaterialsPanel::applyToSelection(const QString& name) {
    if (!session_ || !document_ || session_->selection().mode() != editor::SelectionMode::Faces) {
        emit statusMessage(tr("Select faces to apply a material.")); return;
    }
    std::vector<document::SideId> sides;
    for (const auto target : session_->selection().ordered())
        if (const auto* side = std::get_if<document::SideId>(&target)) sides.push_back(*side);
    if (sides.empty()) { emit statusMessage(tr("No faces selected.")); return; }
    session_->applyMaterial(sides, name.toStdString());
}
QStringList MaterialsPanel::filteredNames() const {
    QStringList names;
    for (int row = 0; row < model_->rowCount(); ++row) names << model_->index(row).data().toString();
    return names;
}
std::size_t MaterialsPanel::thumbnailRequestCount() const { return model_->requests(); }
} // namespace apex::ui
