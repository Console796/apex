#include "ui/cordon_editor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace apex::ui {
namespace {
constexpr double kLimit = 1.0e6;
constexpr const char* kAxes[3] = {"X", "Y", "Z"};
}

CordonEditor::CordonEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Cordons"), this));
    list_ = new QComboBox(this);
    list_->setObjectName(QStringLiteral("cordonList"));
    top->addWidget(list_, 1);
    active_ = new QCheckBox(tr("Active"), this);
    active_->setObjectName(QStringLiteral("cordonActive"));
    top->addWidget(active_);
    add_ = new QPushButton(tr("Add"), this);
    add_->setObjectName(QStringLiteral("cordonAdd"));
    fromSelection_ = new QPushButton(tr("From selection"), this);
    fromSelection_->setObjectName(QStringLiteral("cordonFromSelection"));
    remove_ = new QPushButton(tr("Remove"), this);
    remove_->setObjectName(QStringLiteral("cordonRemove"));
    top->addWidget(add_);
    top->addWidget(fromSelection_);
    top->addWidget(remove_);
    layout->addLayout(top);
    auto* grid = new QGridLayout;
    for (int i = 0; i < 6; ++i) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setObjectName(QStringLiteral("cordon%1%2").arg(i < 3 ? "Min" : "Max").arg(kAxes[i % 3]));
        spin->setRange(-kLimit, kLimit);
        spin->setDecimals(3);
        spin->setKeyboardTracking(false);
        fields_[static_cast<std::size_t>(i)] = spin;
        grid->addWidget(new QLabel(QStringLiteral("%1 %2").arg(i < 3 ? tr("Min") : tr("Max")).arg(kAxes[i % 3]), this),
            i / 3, (i % 3) * 2);
        grid->addWidget(spin, i / 3, (i % 3) * 2 + 1);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { boxEdited(); });
    }
    layout->addLayout(grid);
    connect(list_, &QComboBox::currentIndexChanged, this, [this](int row) { select(row); });
    connect(add_, &QPushButton::clicked, this, [this] { fromSelection(false); });
    connect(fromSelection_, &QPushButton::clicked, this, [this] { fromSelection(true); });
    connect(remove_, &QPushButton::clicked, this, [this] {
        if (const auto id = current()) document::removeCordon(*document_, *id);
    });
    connect(active_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) return;
        if (const auto id = current()) document::setCordonActive(*document_, *id, on);
    });
    reload();
}

CordonEditor::~CordonEditor() {
    if (observing_ && document_) document_->unobserve(token_);
}

void CordonEditor::setDocument(document::Document* document) {
    if (document_ == document) return;
    if (observing_ && document_) document_->unobserve(token_);
    observing_ = false;
    document_ = document;
    if (document_) {
        token_ = document_->observe([this](const document::ChangeSet&) { reload(); });
        observing_ = true;
    }
    reload();
}

std::optional<document::NodeId> CordonEditor::current() const {
    if (!document_ || list_->currentIndex() < 0) return std::nullopt;
    return static_cast<document::NodeId>(list_->currentData().toULongLong());
}

void CordonEditor::reload() {
    const auto keep = current();
    const bool wasUpdating = updating_;
    updating_ = true;
    list_->clear();
    int select = -1;
    if (document_) {
        const auto set = document::readCordons(*document_);
        for (const auto& cordon : set.boxes) {
            const auto name = QString::fromStdString(cordon.name.empty() ? std::string("cordon") : cordon.name);
            list_->addItem(cordon.active ? name : tr("%1 (off)").arg(name), QVariant::fromValue<qulonglong>(cordon.id));
            if (keep && *keep == cordon.id) select = list_->count() - 1;
        }
        if (select < 0 && list_->count() > 0) select = 0;
    }
    list_->setCurrentIndex(select);
    updating_ = wasUpdating;
    this->select(select);
}

void CordonEditor::select(int row) {
    const bool wasUpdating = updating_;
    updating_ = true;
    const auto id = current();
    bool active = false;
    std::array<double, 6> values{};
    if (id && document_ && row >= 0) {
        for (const auto& cordon : document::readCordons(*document_).boxes) {
            if (cordon.id != *id) continue;
            active = cordon.active;
            values = {cordon.box.min.x, cordon.box.min.y, cordon.box.min.z,
                      cordon.box.max.x, cordon.box.max.y, cordon.box.max.z};
        }
    }
    active_->setChecked(active);
    for (std::size_t i = 0; i < 6; ++i) fields_[i]->setValue(values[i]);
    const bool any = id.has_value();
    active_->setEnabled(any);
    remove_->setEnabled(any);
    fromSelection_->setEnabled(any);
    for (auto* field : fields_) field->setEnabled(any);
    add_->setEnabled(document_ != nullptr);
    updating_ = wasUpdating;
}

void CordonEditor::boxEdited() {
    if (updating_) return;
    const auto id = current();
    if (!id) return;
    DVec3 lo{fields_[0]->value(), fields_[1]->value(), fields_[2]->value()};
    DVec3 hi{fields_[3]->value(), fields_[4]->value(), fields_[5]->value()};
    document::setCordonBox(*document_, *id, AABB{glm::min(lo, hi), glm::max(lo, hi)}, "cordon-fields");
}

void CordonEditor::fromSelection(bool replace) {
    if (!document_) return;
    std::optional<AABB> bounds = session_ ? session_->selectionBounds() : std::nullopt;
    if (replace) {
        if (const auto id = current(); id && bounds) document::setCordonBox(*document_, *id, *bounds);
        return;
    }
    document::addCordon(*document_, "cordon", bounds ? *bounds : AABB{DVec3{-256.0}, DVec3{256.0}});
    list_->setCurrentIndex(list_->count() - 1);
}

} // namespace apex::ui
