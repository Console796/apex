#include "ui/transform_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <cmath>
#include <optional>

namespace apex::ui {
namespace {

std::optional<double> parse(const QLineEdit* field) {
    bool ok = false;
    const double value = QLocale::c().toDouble(field->text().trimmed(), &ok);
    if (!ok || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::optional<DVec3> parse3(const QLineEdit* x, const QLineEdit* y, const QLineEdit* z) {
    const auto a = parse(x), b = parse(y), c = parse(z);
    if (!a || !b || !c) return std::nullopt;
    return DVec3{*a, *b, *c};
}

QString text(double value) { return QLocale::c().toString(value, 'g', 12); }

}  // namespace

TransformDialog::TransformDialog(editor::EditorSession& session, QWidget* parent)
    : QDialog(parent), session_(session) {
    setObjectName(QStringLiteral("transformDialog"));
    setWindowTitle(tr("Transform"));
    auto* column = new QVBoxLayout(this);
    tabs_ = new QTabWidget;
    tabs_->setObjectName(QStringLiteral("transformTabs"));
    tabs_->addTab(page(QStringLiteral("move"), {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")}, tr("X"), tr("Y"), tr("Z"), QStringLiteral("0"), move_, &relative_), tr("Move"));
    tabs_->addTab(page(QStringLiteral("rotate"), {QStringLiteral("Pitch"), QStringLiteral("Yaw"), QStringLiteral("Roll")}, tr("Pitch (about Y)"), tr("Yaw (about Z)"), tr("Roll (about X)"),
                       QStringLiteral("0"), rotate_, nullptr), tr("Rotate"));
    tabs_->addTab(page(QStringLiteral("scale"), {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")}, tr("X"), tr("Y"), tr("Z"), QStringLiteral("1"), scale_, nullptr), tr("Scale"));
    column->addWidget(tabs_);
    error_ = new QLabel;
    error_->setObjectName(QStringLiteral("transformError"));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #d9534f;"));
    error_->hide();
    column->addWidget(error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &TransformDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });
    column->addWidget(buttons);
    connect(relative_, &QCheckBox::toggled, this, [this](bool) { fillMove(); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) { showError({}); });
    fillMove();
}

QWidget* TransformDialog::page(const QString& prefix, const QStringList& names, const QString& xLabel, const QString& yLabel,
                               const QString& zLabel, const QString& initial, Fields& fields, QCheckBox** relative) {
    auto* widget = new QWidget;
    auto* form = new QFormLayout(widget);
    const auto add = [&](const QString& suffix, const QString& label, QLineEdit*& field) {
        field = new QLineEdit(initial);
        field->setObjectName(prefix + suffix);
        form->addRow(label, field);
    };
    add(names[0], xLabel, fields.x);
    add(names[1], yLabel, fields.y);
    add(names[2], zLabel, fields.z);
    if (relative) {
        *relative = new QCheckBox(tr("Relative to the current position"));
        (*relative)->setObjectName(QStringLiteral("moveRelative"));
        (*relative)->setToolTip(tr("Off: the selection's pivot moves to these coordinates. On: these offsets are added."));
        form->addRow(*relative);
    }
    return widget;
}

void TransformDialog::fillMove() {
    // Absolute shows the current pivot so an edit of one coordinate leaves the others alone.
    DVec3 value{0.0};
    if (!relative_->isChecked()) {
        if (const auto context = session_.gizmoContext()) value = context->pivot;
    }
    move_.x->setText(text(value.x));
    move_.y->setText(text(value.y));
    move_.z->setText(text(value.z));
}

void TransformDialog::setTab(Tab tab) { tabs_->setCurrentIndex(tab); }
TransformDialog::Tab TransformDialog::tab() const { return static_cast<Tab>(tabs_->currentIndex()); }
QString TransformDialog::error() const { return error_->isHidden() ? QString() : error_->text(); }

void TransformDialog::showError(const QString& message) {
    error_->setText(message);
    error_->setVisible(!message.isEmpty());
}

bool TransformDialog::apply() {
    showError({});
    if (!session_.document() || !session_.gizmoContext()) {
        showError(tr("Nothing is selected to transform."));
        return false;
    }
    if (session_.transformActive()) {
        showError(tr("Finish the active transform first."));
        return false;
    }
    bool ok = false;
    switch (tab()) {
    case Move: {
        const auto value = parse3(move_.x, move_.y, move_.z);
        if (!value) { showError(tr("Move values must be finite numbers.")); return false; }
        ok = session_.moveSelection(*value, relative_->isChecked());
        break;
    }
    case Rotate: {
        const auto value = parse3(rotate_.x, rotate_.y, rotate_.z);
        if (!value) { showError(tr("Rotation angles must be finite numbers (degrees).")); return false; }
        // Pitch turns about Y, yaw about Z, roll about X (Source angles).
        ok = session_.rotateSelection(DVec3{value->z, value->x, value->y});
        break;
    }
    case Scale: {
        const auto value = parse3(scale_.x, scale_.y, scale_.z);
        if (!value) { showError(tr("Scale factors must be finite numbers.")); return false; }
        ok = session_.scaleSelection(*value);
        break;
    }
    }
    if (!ok) {
        showError(tr("These values cannot be applied (for example a zero scale)."));
        return false;
    }
    if (tab() == Move) fillMove();
    return true;
}

void TransformDialog::accept() {
    if (apply()) QDialog::accept();
}

}  // namespace apex::ui
