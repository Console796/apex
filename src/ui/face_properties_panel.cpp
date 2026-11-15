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
#include <QListWidget>
#include <QIntValidator>
#include <QMessageBox>
#include <QLocale>
#include <climits>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <array>
#include <QVBoxLayout>
#include <atomic>

#include "core/strings.hpp"
#include "document/overlay_fragments.hpp"
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
    confirm_ = [this](const QString& title, const QString& text) {
        return QMessageBox::question(this, title, text) == QMessageBox::Yes;
    };
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(2, 2, 2, 2);
    hint_ = new QLabel(tr("Select faces (face selection mode)"), this);
    hint_->setObjectName(QStringLiteral("facePropertiesHint"));
    hint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    hint_->setFixedHeight(fontMetrics().height() + 4);
    layout_->addWidget(hint_);
    // Hammer's Face Edit split: the texture controls on one tab, the displacement's on another, so
    // a displaced face's settings are not below the fold of a scrolled list.
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("facePropertiesTabs"));
    tabs_->setDocumentMode(true);
    layout_->addWidget(tabs_, 1);
    auto* materialPage = new QWidget(tabs_);
    auto* materialPageLayout = new QVBoxLayout(materialPage);
    materialPageLayout->setContentsMargins(0, 2, 0, 0);
    tabs_->addTab(materialPage, tr("Material"));
    uvView_ = new UvView(materialPage);
    materialPageLayout->addWidget(uvView_, 1);
    controlsScroll_ = new QScrollArea(materialPage);
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
    auto* nodraw = new QPushButton(tr("Nodraw"), materialRow);
    nodraw->setObjectName(QStringLiteral("faceMaterialNodraw"));
    nodraw->setToolTip(tr("Apply tools/toolsnodraw to the selected faces"));
    materialLayout->addWidget(material_, 1);
    materialLayout->addWidget(nodraw);
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
    overlaysCaption_ = new QLabel(tr("Overlays on this face"), content_);
    controls->addWidget(overlaysCaption_);
    overlays_ = new QListWidget(content_);
    overlays_->setObjectName(QStringLiteral("faceOverlays"));
    overlays_->setToolTip(tr("Overlays whose side list names this face, and decals placed on it. Double-click to select."));
    overlays_->setMaximumHeight(96);
    overlays_->hide();
    controls->addWidget(overlays_);
    connect(overlays_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!session_ || !item) return;
        auto& selection = session_->selection();
        selection.setMode(editor::SelectionMode::Objects);
        selection.select(static_cast<document::NodeId>(item->data(Qt::UserRole).toULongLong()));
    });
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
    materialPageLayout->addWidget(controlsScroll_);

    auto* displacementScroll = new QScrollArea(tabs_);
    displacementScroll->setWidgetResizable(true);
    displacementScroll->setMinimumSize(0, 0);
    auto* displacementPage = new QWidget(displacementScroll);
    displacementPage->setObjectName(QStringLiteral("faceDisplacementPage"));
    auto* dispControls = new QVBoxLayout(displacementPage);
    dispControls->setContentsMargins(2, 2, 2, 2);
    dispControls->setSpacing(3);
    displacementNone_ = new QLabel(tr("No displaced face is selected. The Displacement tool creates, sews, sculpts "
                                      "and paints displacements."), displacementPage);
    displacementNone_->setObjectName(QStringLiteral("faceDisplacementNone"));
    displacementNone_->setWordWrap(true);
    dispControls->addWidget(displacementNone_);
    displacement_ = new QLabel(displacementPage);
    displacement_->setObjectName(QStringLiteral("faceDisplacement"));
    displacement_->setWordWrap(true);
    displacement_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    displacement_->setToolTip(tr("Create, sew, sculpt and paint with the Displacement tool; Edit > Destroy "
                                 "Displacement makes the faces flat again."));
    displacement_->hide();
    dispControls->addWidget(displacement_);
    displacementEdit_ = new QWidget(displacementPage);
    displacementEdit_->setObjectName(QStringLiteral("faceDisplacementEdit"));
    auto* dispRow = new QHBoxLayout(displacementEdit_);
    dispRow->setContentsMargins(0, 0, 0, 0);
    dispRow->addWidget(new QLabel(tr("Power"), displacementEdit_));
    displacementPower_ = new QComboBox(displacementEdit_);
    displacementPower_->setObjectName(QStringLiteral("faceDisplacementPower"));
    for (int power = geometry::Displacement::kMinPower; power <= geometry::Displacement::kMaxPower; ++power)
        displacementPower_->addItem(QString::number(power), power);
    displacementPower_->setToolTip(tr("The grid's density. A sculpted or painted displacement is resampled: "
                                      "raising keeps its shape exactly, lowering drops the detail in between."));
    dispRow->addWidget(displacementPower_);
    const std::pair<const char*, int> flagBoxes[] = {{"NoPhys", geometry::Displacement::kFlagNoPhys},
                                                     {"NoHull", geometry::Displacement::kFlagNoHull},
                                                     {"NoRay", geometry::Displacement::kFlagNoRay}};
    for (const auto& [name, mask] : flagBoxes) {
        auto* box = new QCheckBox(QString::fromLatin1(name), displacementEdit_);
        box->setObjectName(QStringLiteral("faceDisplacement%1").arg(QLatin1String(name)));
        dispRow->addWidget(box);
        displacementFlags_.emplace_back(box, mask);
        // A click sets the flag on every selected displacement, or clears it when all have it.
        connect(box, &QCheckBox::clicked, this, [this, mask] {
            if (!session_ || rebuilding_) return;
            reportDisplacementEdit(editor::setDisplacementFlags(*session_, shown_, mask, (displacementFlagsAll_ & mask) == 0));
        });
    }
    dispRow->addStretch();
    connect(displacementPower_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        if (!session_ || rebuilding_ || index < 0) return;
        const int power = displacementPower_->itemData(index).toInt();
        if (session_->document() && editor::displacementPowerLosesDetail(*session_->document(), shown_, power) &&
            !confirm_(tr("Lower displacement power"),
                      tr("Lowering the power to %1 drops the sculpted and painted detail between the remaining "
                         "vertices. Undo brings it back.").arg(power))) {
            refresh(); // Back to the current power.
            return;
        }
        reportDisplacementEdit(editor::setDisplacementPower(*session_, shown_, power));
    });
    displacementEdit_->hide();
    dispControls->addWidget(displacementEdit_);
    dispControls->addStretch();
    displacementScroll->setWidget(displacementPage);
    tabs_->addTab(displacementScroll, tr("Displacement"));

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
            if (!self || !self->session_) return;
            self->edit(editor::FaceField::Material, value, nextToken());
            self->session_->setNewSolidMaterial(value.toStdString()); // The browsed texture becomes the active one.
        });
    });
    connect(nodraw, &QPushButton::clicked, this, [this] {
        // The most used texture: one click, and it becomes the active material like a browsed one.
        edit(editor::FaceField::Material, QStringLiteral("tools/toolsnodraw"), nextToken());
        if (session_) session_->setNewSolidMaterial("tools/toolsnodraw");
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
void FacePropertiesPanel::setConfirm(std::function<bool(const QString&, const QString&)> confirm) {
    confirm_ = std::move(confirm);
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

    const auto disp = attached ? editor::summarizeDisplacements(*document_, sides) : editor::DisplacementSummary{};
    // The tab's label counts the displaced faces, so a selection with any shows it from the Material tab.
    tabs_->setTabText(1, disp.displaced ? tr("Displacement (%1)").arg(disp.displaced) : tr("Displacement"));
    displacementNone_->setVisible(disp.displaced == 0);
    displacement_->setVisible(disp.displaced != 0);
    if (disp.displaced != 0) {
        const auto mixed = tr("mixed");
        QStringList lines;
        lines << tr("<b>Displacement</b> (%1 of %2 faces)").arg(disp.displaced).arg(sides.size());
        if (disp.readable != 0) {
            lines << tr("Power: %1").arg(disp.power ? tr("%1 (%2×%2 grid)").arg(*disp.power).arg(disp.gridSize()) : mixed);
            QString flags = mixed;
            if (disp.flags) {
                QStringList names;
                for (const auto& name : editor::displacementFlagNames(*disp.flags)) names << fromStd(name);
                flags = (names.isEmpty() ? tr("none") : names.join(QStringLiteral(", "))) +
                        QStringLiteral(" (%1)").arg(*disp.flags);
            }
            lines << tr("Flags: %1").arg(flags);
            lines << tr("Elevation: %1").arg(disp.elevation ? display(*disp.elevation) : mixed);
            if (disp.alphaMin && disp.alphaMax)
                lines << tr("Alpha: %1 to %2").arg(display(*disp.alphaMin), display(*disp.alphaMax));
        }
        for (const auto& problem : disp.problems) lines << QStringLiteral("⚠ ") + fromStd(problem).toHtmlEscaped();
        displacement_->setText(lines.join(QStringLiteral("<br>")));
    }
    displacementEdit_->setVisible(disp.readable != 0);
    displacementFlagsAll_ = disp.flagsAll;
    {
        const QSignalBlocker block(displacementPower_);
        displacementPower_->setCurrentIndex(disp.power ? displacementPower_->findData(*disp.power) : -1);
    }
    for (const auto& [box, mask] : displacementFlags_) {
        const QSignalBlocker block(box);
        const bool all = (disp.flagsAll & mask) != 0, any = (disp.flagsAny & mask) != 0;
        box->setCheckState(all ? Qt::Checked : any ? Qt::PartiallyChecked : Qt::Unchecked);
    }

    refreshOverlays(enabled && session_->selection().active()
        ? std::optional(std::get<document::SideId>(*session_->selection().active())) : std::nullopt);
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
void FacePropertiesPanel::reportDisplacementEdit(const Result<editor::DisplacementEditReport>& result) {
    emit statusMessage(fromStd(result ? result->message : std::string(result.error().what())));
    requestRefresh();
}
void FacePropertiesPanel::refreshOverlays(std::optional<document::SideId> side) {
    overlays_->clear();
    overlays_->hide(); // Shown only when it has rows, so an empty list takes no room from the UV view.
    if (!side || !document_) {
        overlaysCaption_->setText(tr("Overlays on this face: none"));
        return;
    }
    const auto add = [&](document::NodeId id, bool approximate) {
        const auto handle = document_->handle(id);
        const auto* node = handle ? document_->get(*handle) : nullptr;
        if (!node || !node->block) return;
        const auto value = [&](std::string_view key) {
            const auto* found = node->block->find(key);
            return found ? fromStd(found->decodedValue().value_or("")) : QString{};
        };
        QString label = value("classname");
        if (const auto name = value("targetname"); !name.isEmpty()) label += QStringLiteral(" \"%1\"").arg(name);
        label += QStringLiteral(" #%1").arg(id);
        const auto material = value(approximate ? "texture" : "material");
        if (!material.isEmpty()) label += QStringLiteral(" · ") + material;
        if (approximate) label += tr(" (approximate)");
        auto* item = new QListWidgetItem(label, overlays_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
        item->setToolTip(approximate
            ? tr("Approximate: the engine chooses an infodecal's face at map load; Apex shows it on the nearest face.")
            : tr("Lists this face in its sides. Double-click to select it."));
    };
    for (const auto id : document::overlaysOnSide(*document_, *side)) add(id, false);
    const auto decalSize = [this](std::string_view material) {
        const auto [w, h] = session_ ? session_->faceTextureSize(material) : std::pair{64, 64};
        return DVec2{double(w), double(h)};
    };
    for (const auto id : document::decalsOnSide(*document_, *side, decalSize)) add(id, true);
    overlays_->setVisible(overlays_->count() > 0);
    overlaysCaption_->setText(overlays_->count() ? tr("Overlays on this face (%1)").arg(overlays_->count())
                                                 : tr("Overlays on this face: none"));
}
void FacePropertiesPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!hint_->toolTip().isEmpty())
        hint_->setText(hint_->fontMetrics().elidedText(hint_->toolTip(), Qt::ElideRight, hint_->width()));
}
} // namespace apex::ui
