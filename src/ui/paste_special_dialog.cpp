#include "ui/paste_special_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>
#include <optional>

namespace apex::ui {
namespace {

std::optional<double> parseNumber(const QLineEdit* f) {
    bool ok = false;
    const double v = QLocale::c().toDouble(f->text().trimmed(), &ok);
    if (!ok || !std::isfinite(v)) return std::nullopt;
    return v;
}

}  // namespace

QLineEdit* PasteSpecialDialog::field(const QString& name, const QString& initial) {
    auto* f = new QLineEdit(initial);
    f->setObjectName(name);
    return f;
}

PasteSpecialDialog::PasteSpecialDialog(editor::EditorSession& session, std::string text, editor::PasteOptions base,
                                       QWidget* parent)
    : QDialog(parent), session_(session), text_(std::move(text)), base_(std::move(base)) {
    setObjectName(QStringLiteral("pasteSpecialDialog"));
    setWindowTitle(tr("Paste special"));
    auto* column = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    copies_ = field(QStringLiteral("psCopies"), QStringLiteral("1"));
    copies_->setToolTip(tr("1 to %1. Each copy is the previous one moved and rotated by the values below.")
                            .arg(editor::PasteSpecial::kMaxCopies));
    form->addRow(tr("Copies"), copies_);
    ox_ = field(QStringLiteral("psOffsetX"), QStringLiteral("0"));
    oy_ = field(QStringLiteral("psOffsetY"), QStringLiteral("0"));
    oz_ = field(QStringLiteral("psOffsetZ"), QStringLiteral("0"));
    form->addRow(tr("Offset X"), ox_);
    form->addRow(tr("Offset Y"), oy_);
    form->addRow(tr("Offset Z"), oz_);
    pitch_ = field(QStringLiteral("psPitch"), QStringLiteral("0"));
    yaw_ = field(QStringLiteral("psYaw"), QStringLiteral("0"));
    roll_ = field(QStringLiteral("psRoll"), QStringLiteral("0"));
    form->addRow(tr("Pitch (about Y, degrees)"), pitch_);
    form->addRow(tr("Yaw (about Z, degrees)"), yaw_);
    form->addRow(tr("Roll (about X, degrees)"), roll_);
    group_ = new QCheckBox(tr("Group the copies"));
    group_->setObjectName(QStringLiteral("psGroup"));
    form->addRow(group_);
    names_ = new QComboBox;
    names_->setObjectName(QStringLiteral("psNameMode"));
    names_->addItem(tr("Keep names"), static_cast<int>(editor::NamePolicy::None));
    names_->addItem(tr("Add prefix"), static_cast<int>(editor::NamePolicy::Prefix));
    names_->addItem(tr("Add suffix"), static_cast<int>(editor::NamePolicy::Suffix));
    form->addRow(tr("Entity names"), names_);
    affix_ = field(QStringLiteral("psAffix"), QString());
    affix_->setToolTip(tr("\"%d\" becomes the copy number."));
    form->addRow(tr("Prefix / suffix"), affix_);
    rewrite_ = new QCheckBox(tr("Rewrite references to renamed entities"));
    rewrite_->setObjectName(QStringLiteral("psRewrite"));
    form->addRow(rewrite_);
    column->addLayout(form);
    error_ = new QLabel;
    error_->setObjectName(QStringLiteral("psError"));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #d9534f;"));
    error_->hide();
    column->addWidget(error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &PasteSpecialDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    column->addWidget(buttons);
}

QString PasteSpecialDialog::error() const { return error_->isHidden() ? QString() : error_->text(); }

void PasteSpecialDialog::showError(const QString& message) {
    error_->setText(message);
    error_->setVisible(!message.isEmpty());
}

bool PasteSpecialDialog::apply() {
    showError({});
    if (!session_.document()) {
        showError(tr("No document is open."));
        return false;
    }
    bool ok = false;
    const int copies = copies_->text().trimmed().toInt(&ok);
    if (!ok || copies < 1 || copies > editor::PasteSpecial::kMaxCopies) {
        showError(tr("Copies must be a whole number from 1 to %1.").arg(editor::PasteSpecial::kMaxCopies));
        return false;
    }
    const auto x = parseNumber(ox_), y = parseNumber(oy_), z = parseNumber(oz_);
    if (!x || !y || !z) {
        showError(tr("Offsets must be finite numbers."));
        return false;
    }
    const auto pitch = parseNumber(pitch_), yaw = parseNumber(yaw_), roll = parseNumber(roll_);
    if (!pitch || !yaw || !roll) {
        showError(tr("Rotation angles must be finite numbers (degrees)."));
        return false;
    }
    editor::PasteSpecial special;
    special.copies = copies;
    special.offset = DVec3{*x, *y, *z};
    special.rotationDeg = DVec3{*roll, *pitch, *yaw};  // About X, Y, Z (Source: roll, pitch, yaw).
    special.groupCopies = group_->isChecked();
    special.names = static_cast<editor::NamePolicy>(names_->currentData().toInt());
    special.affix = affix_->text().toStdString();
    if (special.names != editor::NamePolicy::None && special.affix.empty()) {
        showError(tr("Enter the prefix or suffix to add to entity names."));
        return false;
    }
    if (special.affix.find_first_of("\"\n\r") != std::string::npos) {
        showError(tr("The prefix or suffix cannot contain quotes or line breaks."));
        return false;
    }
    special.rewriteReferences = rewrite_->isChecked();
    auto options = base_;
    options.special = special;
    if (auto done = session_.pasteText(text_, options); !done) {
        showError(QString::fromStdString(done.error().what()));
        return false;
    }
    return true;
}

void PasteSpecialDialog::accept() {
    if (apply()) QDialog::accept();
}

}  // namespace apex::ui
