#include "ui/face_properties_panel.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleValidator>
#include <QEvent>
#include <QImage>
#include <QFormLayout>
#include <QPixmap>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QIntValidator>
#include <QLocale>
#include <climits>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <array>
#include <QVBoxLayout>
#include <atomic>

#include "core/strings.hpp"
#include "ui/uv_view.hpp"
#include "ui/game_content.hpp"
#include "ui/theme.hpp"

namespace apex::ui {
namespace {
std::atomic<std::uint64_t> nextFocus{1};
QString nextToken() { return QString::number(nextFocus++); }
QString fromStd(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QString display(double value) { return QString::number(value, 'g', 12); }
}

FacePropertiesPanel::FacePropertiesPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("facePropertiesPanel"));
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(2, 2, 2, 2);
    hint_ = new QLabel(tr("Select faces (face selection mode)"), this);
    hint_->setObjectName(QStringLiteral("facePropertiesHint"));
    hint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    hint_->setFixedHeight(fontMetrics().height() + 4);
    layout_->addWidget(hint_);
    uvView_ = new UvView(this);
    layout_->addWidget(uvView_, 1);
    controlsScroll_ = new QScrollArea(this);
    controlsScroll_->setWidgetResizable(true);
    controlsScroll_->setMinimumSize(0, 0);
    controlsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    content_ = new QWidget(controlsScroll_);
    content_->setObjectName(QStringLiteral("facePropertiesContent"));
    auto* controls = new QVBoxLayout(content_);
    controls->setContentsMargins(2, 2, 2, 2);
    controls->setSpacing(3);
    auto* materialRow = new QWidget(content_);
    auto* materialLayout = new QHBoxLayout(materialRow);
    materialLayout->setContentsMargins(0, 0, 0, 0);
    auto* swatch = new QLabel(materialRow);
    swatch->setObjectName(QStringLiteral("faceMaterialSwatch"));
    swatch->setFixedSize(22, 22);
    swatch->setAutoFillBackground(true);
    swatch->setStyleSheet(QStringLiteral("background: #606368; border: 1px solid palette(mid);"));
    materialLayout->addWidget(swatch);
    material_ = new QLineEdit(materialRow);
    material_->setObjectName(QStringLiteral("faceMaterial"));
    auto* browse = new QPushButton(tr("Browse…"), materialRow);
    browse->setObjectName(QStringLiteral("faceMaterialBrowse"));
    materialLayout->addWidget(material_, 1);
    materialLayout->addWidget(browse);
    controls->addWidget(materialRow);

    auto* editorRow = new QHBoxLayout;
    auto* alignmentColumn = new QVBoxLayout;
    auto* square = new QWidget(content_);
    square->setObjectName(QStringLiteral("faceAlignmentSquare"));
    auto* squareGrid = new QGridLayout(square);
    squareGrid->setContentsMargins(0, 0, 0, 0);
    squareGrid->setSpacing(1);
    struct Cell { const char* id; const char* glyph; geometry::HorizontalAlignment h; geometry::VerticalAlignment v; };
    const Cell cells[] = {
        {"tl", "↖", geometry::HorizontalAlignment::Left, geometry::VerticalAlignment::Top},
        {"t", "↑", geometry::HorizontalAlignment::Center, geometry::VerticalAlignment::Top},
        {"tr", "↗", geometry::HorizontalAlignment::Right, geometry::VerticalAlignment::Top},
        {"l", "←", geometry::HorizontalAlignment::Left, geometry::VerticalAlignment::Center},
        {"c", "•", geometry::HorizontalAlignment::Center, geometry::VerticalAlignment::Center},
        {"r", "→", geometry::HorizontalAlignment::Right, geometry::VerticalAlignment::Center},
        {"bl", "↙", geometry::HorizontalAlignment::Left, geometry::VerticalAlignment::Bottom},
        {"b", "↓", geometry::HorizontalAlignment::Center, geometry::VerticalAlignment::Bottom},
        {"br", "↘", geometry::HorizontalAlignment::Right, geometry::VerticalAlignment::Bottom}};
    for (int i = 0; i < 9; ++i) {
        const auto cell = cells[i];
        auto* button = new QToolButton(square);
        button->setObjectName(QStringLiteral("faceAlign.%1").arg(QLatin1String(cell.id)));
        button->setText(QString::fromUtf8(cell.glyph));
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFixedSize(26, 24);
        squareGrid->addWidget(button, i / 3, i % 3);
        alignment_.push_back(button);
        connect(button, &QToolButton::clicked, this, [this, cell] {
            if (session_) if (auto result = session_->justifyFaces(cell.h, cell.v, treatAsOne_->isChecked()); !result)
                emit statusMessage(fromStd(result.error().what()));
        });
    }
    alignmentColumn->addWidget(square);
    auto* commandRow = new QHBoxLayout;
    auto* fit = new QPushButton(tr("Fit"), content_);
    fit->setObjectName(QStringLiteral("faceJustify.fit"));
    commandRow->addWidget(fit);
    treatAsOne_ = new QCheckBox(tr("Treat as one"), content_);
    treatAsOne_->setObjectName(QStringLiteral("faceTreatAsOne"));
    commandRow->addWidget(treatAsOne_);
    alignmentColumn->addLayout(commandRow);
    editorRow->addLayout(alignmentColumn);
    connect(fit, &QPushButton::clicked, this, [this] {
        if (session_) if (auto result = session_->justifyFaces(geometry::Justify::Fit, treatAsOne_->isChecked()); !result)
            emit statusMessage(fromStd(result.error().what()));
    });

    auto* fields = new QGridLayout;
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setHorizontalSpacing(4);
    fields->setVerticalSpacing(2);
    const auto numberField = [&](QLineEdit*& field, const char* objectName) {
        field = new QLineEdit(content_);
        field->setObjectName(QString::fromLatin1(objectName));
        auto* validator = new QDoubleValidator(-1e12, 1e12, 12, field);
        validator->setNotation(QDoubleValidator::ScientificNotation);
        validator->setLocale(QLocale::c());
        field->setValidator(validator);
        field->setMaximumWidth(fontMetrics().horizontalAdvance(QLatin1Char('0')) * 9 + 14);
    };
    numberField(shiftU_, "faceShiftU"); numberField(shiftV_, "faceShiftV");
    numberField(scaleU_, "faceScaleU"); numberField(scaleV_, "faceScaleV");
    numberField(rotation_, "faceRotation");
    lightmap_ = new QLineEdit(content_);
    lightmap_->setObjectName(QStringLiteral("faceLightmapScale"));
    auto* lightmapValidator = new QIntValidator(1, INT_MAX, lightmap_);
    lightmapValidator->setLocale(QLocale::c()); lightmap_->setValidator(lightmapValidator);
    lightmap_->setMaximumWidth(fontMetrics().horizontalAdvance(QLatin1Char('0')) * 9 + 14);
    const auto field = [&](int row, int col, const QString& label, QLineEdit* edit) {
        auto* caption = new QLabel(label, content_);
        fields->addWidget(caption, row, col * 2);
        fields->addWidget(edit, row, col * 2 + 1);
    };
    field(0, 0, tr("U shift"), shiftU_); field(0, 1, tr("V shift"), shiftV_);
    field(1, 0, tr("U scale"), scaleU_); field(1, 1, tr("V scale"), scaleV_);
    field(2, 0, tr("Rotation"), rotation_); field(2, 1, tr("Lightmap"), lightmap_);
    editorRow->addLayout(fields, 1);
    controls->addLayout(editorRow);

    auto* alignRow = new QHBoxLayout;
    auto* world = new QPushButton(tr("World"), content_);
    world->setObjectName(QStringLiteral("faceAlignWorld"));
    auto* face = new QPushButton(tr("Face"), content_);
    face->setObjectName(QStringLiteral("faceAlignFace"));
    alignRow->addWidget(world); alignRow->addWidget(face);
    smoothingSummary_ = new QToolButton(content_);
    smoothingSummary_->setObjectName(QStringLiteral("faceSmoothingSummary"));
    smoothingSummary_->setText(tr("Smoothing: none"));
    smoothingSummary_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    smoothingSummary_->setAutoRaise(true);
    alignRow->addWidget(smoothingSummary_);
    extrapolation_ = new QComboBox(content_);
    extrapolation_->setObjectName(QStringLiteral("faceExtrapolation"));
    extrapolation_->addItem(tr("Wrap"), static_cast<int>(editor::FaceExtrapolation::Wrap));
    extrapolation_->addItem(tr("Projected"), static_cast<int>(editor::FaceExtrapolation::Projected));
    extrapolation_->addItem(tr("Match"), static_cast<int>(editor::FaceExtrapolation::Match));
    extrapolation_->setToolTip(tr("Texture alignment when applying a material to another face."));
    alignRow->addWidget(extrapolation_);
    alignRow->addStretch();
    controls->addLayout(alignRow);
    auto* smoothingGrid = new QWidget(content_);
    smoothingGrid->setObjectName(QStringLiteral("faceSmoothingGroups"));
    auto* bits = new QGridLayout(smoothingGrid);
    bits->setContentsMargins(0, 0, 0, 0); bits->setSpacing(1);
    for (unsigned bit = 0; bit < 32; ++bit) {
        auto* button = new QToolButton(smoothingGrid);
        button->setText(QString::number(bit + 1));
        button->setObjectName(QStringLiteral("smoothingGroup.%1").arg(bit + 1));
        button->setCheckable(true); button->setAutoRaise(true); button->setFixedSize(25, 21);
        bits->addWidget(button, static_cast<int>(bit / 8), static_cast<int>(bit % 8));
        smoothing_.push_back(button);
        connect(button, &QToolButton::clicked, this, [this, bit](bool checked) {
            if (!session_ || rebuilding_) return;
            auto result = session_->toggleFaceSmoothingGroup(std::uint32_t{1} << bit, checked);
            if (!result) emit statusMessage(fromStd(result.error().what())); else requestRefresh();
        });
    }
    smoothingGrid->hide();
    controls->addWidget(smoothingGrid);
    connect(smoothingSummary_, &QToolButton::clicked, smoothingGrid, [smoothingGrid] { smoothingGrid->setVisible(!smoothingGrid->isVisible()); });
    connect(world, &QPushButton::clicked, this, [this] {
        if (session_) if (auto result = session_->alignFaces(editor::FaceAlignment::World); !result)
            emit statusMessage(fromStd(result.error().what()));
    });
    connect(face, &QPushButton::clicked, this, [this] {
        if (session_) if (auto result = session_->alignFaces(editor::FaceAlignment::Face); !result)
            emit statusMessage(fromStd(result.error().what()));
    });
    connect(extrapolation_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (session_ && index >= 0)
            session_->setFaceExtrapolation(static_cast<editor::FaceExtrapolation>(extrapolation_->itemData(index).toInt()));
    });
    controlsScroll_->setWidget(content_);
    layout_->addWidget(controlsScroll_);

    const auto connectText = [&](QLineEdit* field, editor::FaceField kind) {
        field->installEventFilter(this);
        connect(field, &QLineEdit::textEdited, this, [this, field, kind](const QString& value) {
            if (field->hasAcceptableInput()) edit(kind, value, field->property("faceFocus").toString());
        });
        connect(field, &QLineEdit::editingFinished, this, &FacePropertiesPanel::requestRefresh);
    };
    // A material is written when the field is committed, not per keystroke: each partial name would
    // be a material load and an edit.
    material_->installEventFilter(this);
    connect(material_, &QLineEdit::editingFinished, this, [this] {
        if (!material_->isModified() || material_->text().isEmpty()) return requestRefresh();
        material_->setModified(false);
        edit(editor::FaceField::Material, material_->text(), nextToken());
        requestRefresh();
    });
    connectText(shiftU_, editor::FaceField::ShiftU);
    connectText(shiftV_, editor::FaceField::ShiftV);
    connectText(scaleU_, editor::FaceField::ScaleU);
    connectText(scaleV_, editor::FaceField::ScaleV);
    connectText(rotation_, editor::FaceField::Rotation);
    connectText(lightmap_, editor::FaceField::LightmapScale);
    connect(browse, &QPushButton::clicked, this, [this] {
        if (!materialBrowser_ || !material_) return;
        QPointer<FacePropertiesPanel> self(this);
        materialBrowser_(material_->text(), tr("the selected faces"), [self](QString value) {
            if (self && self->session_) self->edit(editor::FaceField::Material, value, nextToken());
        });
    });
    connect(uvView_, &UvView::statusMessage, this, &FacePropertiesPanel::statusMessage);
    refresh();
}
FacePropertiesPanel::~FacePropertiesPanel() {
    setEditorSession(nullptr);
    setDocument(nullptr);
}
void FacePropertiesPanel::setDocument(document::Document* document) {
    if (document_) document_->unobserve(documentObserver_);
    document_ = document;
    uvView_->setDocument(document);
    if (document_) documentObserver_ = document_->observe([this](const document::ChangeSet&) { requestRefresh(); });
    requestRefresh();
}
void FacePropertiesPanel::setEditorSession(editor::EditorSession* session) {
    if (session_) session_->unobserve(sessionObserver_);
    session_ = session;
    uvView_->setEditorSession(session);
    if (session_) sessionObserver_ = session_->observe([this] { requestRefresh(); });
    requestRefresh();
}
void FacePropertiesPanel::setGameContent(GameContent* content) {
    QObject::disconnect(thumbnailConnection_);
    QObject::disconnect(contentConnection_);
    gameContent_ = content;
    uvView_->setGameContent(content);
    if (gameContent_) {
        thumbnailConnection_ = connect(gameContent_, &GameContent::thumbnailsReady, this, &FacePropertiesPanel::requestRefresh);
        contentConnection_ = connect(gameContent_, &GameContent::contentChanged, this, &FacePropertiesPanel::requestRefresh);
    }
    requestRefresh();
}
void FacePropertiesPanel::setMaterialBrowser(
    std::function<void(QString, QString, std::function<void(QString)>)> browser) {
    materialBrowser_ = std::move(browser);
    if (auto* button = findChild<QPushButton*>(QStringLiteral("faceMaterialBrowse")))
        button->setEnabled(static_cast<bool>(materialBrowser_));
}
bool FacePropertiesPanel::fieldHasFocus() const {
    auto* focus = QApplication::focusWidget();
    return focus && (focus == material_ || focus == shiftU_ || focus == shiftV_ || focus == scaleU_ ||
                     focus == scaleV_ || focus == rotation_ || focus == lightmap_);
}
bool FacePropertiesPanel::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::FocusIn) object->setProperty("faceFocus", nextToken());
    if (event->type() == QEvent::FocusOut) requestRefresh();
    return QWidget::eventFilter(object, event);
}
void FacePropertiesPanel::requestRefresh() {
    if (queued_) return;
    queued_ = true;
    QTimer::singleShot(0, this, [this] { queued_ = false; refresh(); });
}
void FacePropertiesPanel::edit(editor::FaceField field, const QString& value, const QString& token) {
    if (!session_) return;
    bool ok = true;
    editor::FaceValue typed = field == editor::FaceField::Material
        ? editor::FaceValue{value.toStdString()} : editor::FaceValue{value.toDouble(&ok)};
    if (!ok) return;
    auto result = session_->setFaceField(field, std::move(typed), token.toStdString());
    if (!result) emit statusMessage(fromStd(result.error().what()));
}
void FacePropertiesPanel::refresh() {
    if (rebuilding_) return;
    const bool attached = session_ && document_ && session_->document() == document_;
    const bool canEdit = attached && session_->selection().mode() == editor::SelectionMode::Faces &&
                         !session_->selection().ordered().empty();
    content_->setEnabled(canEdit);
    if (session_) {
        const QSignalBlocker modeBlock(extrapolation_);
        extrapolation_->setCurrentIndex(extrapolation_->findData(static_cast<int>(session_->faceExtrapolation())));
        const auto mode = session_->faceExtrapolation();
        extrapolation_->setToolTip(mode == editor::FaceExtrapolation::Wrap
            ? tr("Fold texture axes around the hinge between faces.")
            : mode == editor::FaceExtrapolation::Projected
                ? tr("Continue the source axes straight across; degenerate axes use Wrap.")
                : tr("Align to the target face and copy source rotation and scale."));
    }
    if (!canEdit) {
        hint_->setToolTip(tr("Select faces (face selection mode)"));
        hint_->setText(hint_->fontMetrics().elidedText(hint_->toolTip(), Qt::ElideRight, hint_->width()));
    }
    std::vector<document::SideId> sides;
    if (attached && session_->selection().mode() == editor::SelectionMode::Faces)
        for (const auto target : session_->selection().ordered())
            sides.push_back(std::get<document::SideId>(target));
    // A focused field keeps its text while the user types, unless the selection changed under it:
    // then it shows the new faces and starts a new typing step, so text meant for A never edits B.
    if (fieldHasFocus()) {
        if (sides == shown_) return;
        if (auto* focus = QApplication::focusWidget()) focus->setProperty("faceFocus", nextToken());
    }
    shown_ = sides;
    rebuilding_ = true;
    const auto properties = attached ? editor::summarize(*document_, sides) : editor::FaceProperties{};
    const bool enabled = properties.count != 0;
    content_->setEnabled(enabled);
    hint_->setToolTip(enabled ? tr("%1 face(s) selected").arg(properties.count)
                              : tr("Select faces (face selection mode)"));
    hint_->setText(hint_->fontMetrics().elidedText(hint_->toolTip(), Qt::ElideRight, hint_->width()));
    const auto set = [](QLineEdit* field, const auto& value) {
        const QSignalBlocker block(field);
        field->clear();
        field->setPlaceholderText(value ? QString{} : QObject::tr("Mixed"));
        if (value) field->setText(display(*value));
    };
    const QSignalBlocker materialBlock(material_);
    material_->clear();
    material_->setPlaceholderText(properties.material ? QString{} : tr("Mixed"));
    if (properties.material) material_->setText(fromStd(*properties.material));
    set(shiftU_, properties.shiftU); set(shiftV_, properties.shiftV);
    set(scaleU_, properties.scaleU); set(scaleV_, properties.scaleV);
    set(rotation_, properties.rotation); set(lightmap_, properties.lightmapScale);
    // Bits set on every face, and bits set on some: one snapshot per face, not per bit.
    std::uint32_t all = sides.empty() ? 0 : ~std::uint32_t{0}, any = 0;
    for (auto side : sides) if (const auto face = editor::faceSnapshot(*document_, side)) {
        all &= face->face.smoothingGroups; any |= face->face.smoothingGroups;
    }
    for (std::size_t i = 0; i < smoothing_.size(); ++i) {
        auto* button = smoothing_[i];
        const std::uint32_t mask = std::uint32_t{1} << i;
        const bool value = (all & mask) != 0, mixed = !value && (any & mask) != 0;
        const QSignalBlocker block(button);
        button->setChecked(value);
        button->setProperty("mixed", mixed);
        button->setStyleSheet(mixed ? QStringLiteral("QToolButton[mixed=\"true\"] { border: 1px dashed palette(highlight); }") : QString{});
    }
    QStringList labels;
    for (unsigned bit = 0; bit < 32; ++bit) if ((all & (std::uint32_t{1} << bit)) != 0)
        labels << QString::number(bit + 1);
    const bool mixedGroups = all != any;
    const auto summary = mixedGroups ? tr("Smoothing: mixed") : labels.empty() ? tr("Smoothing: none")
                                                    : tr("Smoothing: %1").arg(labels.join(QStringLiteral(", ")));
    smoothingSummary_->setText(summary);
    smoothingSummary_->setToolTip(summary);

    if (enabled && session_->selection().active()) {
        const auto active = std::get<document::SideId>(*session_->selection().active());
        if (const auto face = editor::faceSnapshot(*document_, active)) {
            const auto [w, h] = session_->faceTextureSize(face->face.material);
            const auto alignment = geometry::alignmentOf(face->face.uv, face->points, w, h);
            const auto cells = std::array{
                geometry::Alignment{geometry::HorizontalAlignment::Left, geometry::VerticalAlignment::Top},
                geometry::Alignment{geometry::HorizontalAlignment::Center, geometry::VerticalAlignment::Top},
                geometry::Alignment{geometry::HorizontalAlignment::Right, geometry::VerticalAlignment::Top},
                geometry::Alignment{geometry::HorizontalAlignment::Left, geometry::VerticalAlignment::Center},
                geometry::Alignment{geometry::HorizontalAlignment::Center, geometry::VerticalAlignment::Center},
                geometry::Alignment{geometry::HorizontalAlignment::Right, geometry::VerticalAlignment::Center},
                geometry::Alignment{geometry::HorizontalAlignment::Left, geometry::VerticalAlignment::Bottom},
                geometry::Alignment{geometry::HorizontalAlignment::Center, geometry::VerticalAlignment::Bottom},
                geometry::Alignment{geometry::HorizontalAlignment::Right, geometry::VerticalAlignment::Bottom}};
            for (std::size_t i = 0; i < alignment_.size(); ++i) {
                const QSignalBlocker block(alignment_[i]);
                alignment_[i]->setChecked(alignment.horizontal == cells[i].horizontal && alignment.vertical == cells[i].vertical);
            }
            if (auto* swatch = findChild<QLabel*>(QStringLiteral("faceMaterialSwatch"))) {
                QPixmap pixmap(22, 22);
                pixmap.fill(currentTokens().raised);
                if (gameContent_ && gameContent_->thumbnails())
                    if (const auto thumb = gameContent_->thumbnails()->request(face->face.material, 22))
                        pixmap = QPixmap::fromImage(QImage(reinterpret_cast<const uchar*>(thumb->rgba.data()),
                            thumb->width, thumb->height, thumb->width * 4, QImage::Format_RGBA8888)
                            .scaled(22, 22, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
                swatch->setPixmap(pixmap);
            }
        }
    } else {
        for (auto* button : alignment_) { const QSignalBlocker block(button); button->setChecked(false); }
    }
    rebuilding_ = false;
}
void FacePropertiesPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!hint_->toolTip().isEmpty())
        hint_->setText(hint_->fontMetrics().elidedText(hint_->toolTip(), Qt::ElideRight, hint_->width()));
}
} // namespace apex::ui
