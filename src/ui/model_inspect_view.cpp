#include "ui/model_inspect_view.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace apex::ui {
// Paints the render square-fitted on a neutral ground; left drag orbits, right or middle drag pans,
// the wheel zooms about the centre and a double click returns to the first view.
class ModelInspectCanvas final : public QWidget {
public:
    explicit ModelInspectCanvas(ModelInspectView& owner) : QWidget(&owner), owner_(owner) {
        setObjectName(QStringLiteral("modelInspectCanvas"));
        setMinimumSize(96, 96);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::OpenHandCursor);
        setToolTip(tr("Drag to turn, right-drag to pan, scroll to zoom, double-click to reset"));
    }
    void setImage(QImage image) { image_ = std::move(image); update(); }
    void setStatus(QString status) { if (status != status_) { status_ = std::move(status); update(); } }
    const QImage& image() const { return image_; }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(52, 54, 58));
        const int side = std::min(width(), height());
        const QRect square((width() - side) / 2, (height() - side) / 2, side, side);
        if (!image_.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(square, image_);
        }
        if (!status_.isEmpty()) {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap, status_);
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        last_ = event->position();
        if (event->button() == Qt::LeftButton) setCursor(Qt::ClosedHandCursor);
    }
    void mouseReleaseEvent(QMouseEvent*) override { setCursor(Qt::OpenHandCursor); }
    void mouseMoveEvent(QMouseEvent* event) override {
        const auto delta = event->position() - last_;
        last_ = event->position();
        auto& view = owner_.view_;
        if (event->buttons() & Qt::LeftButton) {
            view.yaw = std::fmod(view.yaw - delta.x() * 0.5 + 360.0, 360.0);
            view.pitch = std::clamp(view.pitch + delta.y() * 0.5, -89.0, 89.0);
        } else if (event->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
            // The render spans 2.2 radii of the sphere at zoom 1.
            const double perPixel = 2.2 / (std::max(1, std::min(width(), height())) * view.zoom);
            view.pan.x -= delta.x() * perPixel;
            view.pan.y += delta.y() * perPixel;
        } else return;
        emit owner_.viewChanged();
    }
    void wheelEvent(QWheelEvent* event) override {
        const double steps = event->angleDelta().y() / 120.0;
        if (steps == 0.0) return;
        owner_.view_.zoom = std::clamp(owner_.view_.zoom * std::pow(1.15, steps), 0.25, 32.0);
        emit owner_.viewChanged();
        event->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { owner_.resetCamera(); }
private:
    ModelInspectView& owner_;
    QImage image_;
    QString status_;
    QPointF last_;
};

ModelInspectView::ModelInspectView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("modelInspector"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    canvas_ = new ModelInspectCanvas(*this);
    layout->addWidget(canvas_, 3);
    auto* side = new QVBoxLayout;
    info_ = new QLabel(this);
    info_->setObjectName(QStringLiteral("modelInspectInfo"));
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_->setWordWrap(true);
    info_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    side->addWidget(info_);
    choices_ = new QFormLayout;
    skin_ = new QSpinBox(this);
    skin_->setObjectName(QStringLiteral("modelInspectSkin"));
    skinLabel_ = new QLabel(tr("Skin"), this);
    choices_->addRow(skinLabel_, skin_);
    side->addLayout(choices_);
    materials_ = new QListWidget(this);
    materials_->setObjectName(QStringLiteral("modelInspectMaterials"));
    materials_->setSelectionMode(QAbstractItemView::NoSelection);
    materials_->setTextElideMode(Qt::ElideLeft);
    materials_->setMinimumHeight(48);
    side->addWidget(materials_, 1);
    reset_ = new QPushButton(tr("Reset view"), this);
    reset_->setObjectName(QStringLiteral("modelInspectReset"));
    side->addWidget(reset_);
    layout->addLayout(side, 2);
    connect(reset_, &QPushButton::clicked, this, &ModelInspectView::resetCamera);
    connect(skin_, &QSpinBox::valueChanged, this, [this] { changeChoice(); });
    skin_->setRange(0, 0);
    skinLabel_->hide();
    skin_->hide();
    updateText();
}

void ModelInspectView::resetCamera() {
    view_.yaw = render::ModelView{}.yaw;
    view_.pitch = render::ModelView{}.pitch;
    view_.zoom = 1.0;
    view_.pan = {};
    emit viewChanged();
}

void ModelInspectView::setModel(const Summary& summary) {
    if (summary.name == summary_.name) { summary_ = summary; updateText(); return; }
    summary_ = summary;
    view_.zoom = 1.0;
    view_.pan = {};
    view_.body = view_.skin = 0;
    haveStats_ = false;
    stats_ = {};
    revision_ = ~std::uint64_t{0};
    canvas_->setImage({});
    canvas_->setStatus(tr("Loading…"));
    rebuildChoices(stats_);
    materials_->clear();
    updateText();
    emit viewChanged();
}
void ModelInspectView::clearModel() {
    summary_ = {};
    view_.zoom = 1.0;
    view_.pan = {};
    view_.body = view_.skin = 0;
    haveStats_ = false;
    stats_ = {};
    revision_ = ~std::uint64_t{0};
    canvas_->setImage({});
    canvas_->setStatus(tr("Select a model to inspect it"));
    rebuildChoices(stats_);
    materials_->clear();
    updateText();
}

void ModelInspectView::setInspection(const render::ModelInspection* inspection) {
    if (!hasModel()) return;
    if (!inspection) {
        canvas_->setStatus(tr("Preview unavailable: no GPU or game content"));
        return;
    }
    using State = render::ModelInspection::State;
    if (inspection->revision == revision_) return;
    revision_ = inspection->revision;
    if (inspection->image) {
        const auto& image = *inspection->image;
        canvas_->setImage(QImage(reinterpret_cast<const uchar*>(image.rgba.data()), image.width, image.height,
                                 image.width * 4, QImage::Format_RGBA8888).copy());
    }
    switch (inspection->state) {
    case State::Idle: case State::Loading: canvas_->setStatus(inspection->image ? QString{} : tr("Loading…")); break;
    case State::Ready: canvas_->setStatus({}); break;
    case State::Failed:
        canvas_->setStatus(tr("Cannot show this model: %1").arg(
            inspection->error ? QString::fromUtf8(inspection->error->what().data(), static_cast<qsizetype>(inspection->error->what().size())) : QString{}));
        break;
    }
    if (inspection->state == State::Ready) {
        haveStats_ = true;
        stats_ = inspection->stats;
        rebuildChoices(stats_);
        materials_->clear();
        for (const auto& material : stats_.materials) {
            auto text = QString::fromStdString(material.name) + QStringLiteral("  ·  ") + tr("%n triangle(s)", "", static_cast<int>(material.triangles));
            if (!material.resolved) text += QStringLiteral("  ·  ") + tr("not found");
            else if (material.state == assets::MaterialState::Failed) text += QStringLiteral("  ·  ") + tr("failed to load");
            else if (material.state != assets::MaterialState::Ready) text += QStringLiteral("  ·  ") + tr("loading");
            materials_->addItem(text);
        }
        updateText();
    }
}

void ModelInspectView::rebuildChoices(const render::ModelInspectStats& stats) {
    const auto families = static_cast<int>(stats.skinFamilies);
    const QSignalBlocker block(skin_);
    skin_->setRange(0, std::max(0, families - 1));
    skin_->setValue(std::min<int>(static_cast<int>(view_.skin), skin_->maximum()));
    skin_->setSuffix(families > 1 ? tr(" of %1").arg(families - 1) : QString{});
    skinLabel_->setVisible(families > 1);
    skin_->setVisible(families > 1);
    // Rebuild the body group combos only when the model's groups differ from those shown.
    bool same = bodyCombos_.size() == stats.bodyParts.size();
    for (std::size_t i = 0; same && i < bodyCombos_.size(); ++i)
        same = bodyCombos_[i]
            ? bodyCombos_[i]->count() == static_cast<int>(stats.bodyParts[i].choices.size()) &&
                  bodyCombos_[i]->property("part").toString() == QString::fromStdString(stats.bodyParts[i].name)
            : stats.bodyParts[i].choices.size() < 2; // A null entry is a part with nothing to choose.
    if (!same) {
        for (auto* combo : bodyCombos_) if (combo) choices_->removeRow(combo);
        bodyCombos_.clear();
        for (std::size_t i = 0; i < stats.bodyParts.size(); ++i) {
            const auto& part = stats.bodyParts[i];
            if (part.choices.size() < 2) { bodyCombos_.push_back(nullptr); continue; }
            auto* combo = new QComboBox(this);
            combo->setObjectName(QStringLiteral("modelInspectBody%1").arg(i));
            combo->setProperty("part", QString::fromStdString(part.name));
            for (const auto& choice : part.choices) combo->addItem(QString::fromStdString(choice));
            choices_->addRow(QString::fromStdString(part.name.empty() ? "Body " + std::to_string(i) : part.name), combo);
            connect(combo, &QComboBox::currentIndexChanged, this, [this] { changeChoice(); });
            bodyCombos_.push_back(combo);
        }
        // A null entry stands for a part with nothing to choose, keeping indices aligned with bodyParts.
    }
    for (std::size_t i = 0; i < bodyCombos_.size() && i < stats.bodyParts.size(); ++i) {
        if (!bodyCombos_[i]) continue;
        const QSignalBlocker blocker(bodyCombos_[i]);
        const auto& part = stats.bodyParts[i];
        bodyCombos_[i]->setCurrentIndex(static_cast<int>((view_.body / part.base) % part.choices.size()));
    }
}

void ModelInspectView::changeChoice() {
    view_.skin = static_cast<std::uint32_t>(skin_->value());
    std::uint32_t body = 0;
    for (std::size_t i = 0; i < bodyCombos_.size() && i < stats_.bodyParts.size(); ++i)
        if (bodyCombos_[i]) body += static_cast<std::uint32_t>(bodyCombos_[i]->currentIndex()) * stats_.bodyParts[i].base;
    view_.body = body;
    emit viewChanged();
}

void ModelInspectView::updateText() {
    if (!hasModel()) { info_->setText({}); return; }
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(summary_.name.toHtmlEscaped());
    if (haveStats_) {
        lines << tr("%L1 triangles, %L2 vertices").arg(stats_.triangles).arg(stats_.vertices);
        lines << tr("Size %1 × %2 × %3 units").arg(stats_.size.x, 0, 'f', 1).arg(stats_.size.y, 0, 'f', 1).arg(stats_.size.z, 0, 'f', 1);
    } else {
        lines << tr("Hull %1 × %2 × %3 units").arg(summary_.hullSize[0], 0, 'f', 1).arg(summary_.hullSize[1], 0, 'f', 1).arg(summary_.hullSize[2], 0, 'f', 1);
    }
    lines << tr("%1 body parts; %2 skin families, %3 skins per family").arg(summary_.bodyParts).arg(summary_.skinFamilies).arg(summary_.skinsPerFamily);
    lines << (summary_.staticProp ? tr("Static prop") : tr("Dynamic model")) + QStringLiteral(" · ") + summary_.source.toHtmlEscaped();
    info_->setText(lines.join(QStringLiteral("<br>")));
}
} // namespace apex::ui
