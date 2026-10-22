#include "ui/tie_dialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace apex::ui {

TieDialog::TieDialog(editor::EditorSession& session, QStringList classes, QWidget* parent)
    : QDialog(parent), session_(session) {
    setObjectName(QStringLiteral("tieDialog"));
    setWindowTitle(tr("Tie to entity"));
    auto* column = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    class_ = new QComboBox;
    class_->setObjectName(QStringLiteral("tieClass"));
    class_->setEditable(true);
    class_->setInsertPolicy(QComboBox::NoInsert);
    classes.removeAll(QStringLiteral("func_detail"));
    classes.sort(Qt::CaseInsensitive);
    class_->addItem(QStringLiteral("func_detail"));
    class_->addItems(classes);
    class_->setCurrentIndex(0);
    form->addRow(tr("Entity class"), class_);
    column->addLayout(form);
    error_ = new QLabel;
    error_->setObjectName(QStringLiteral("tieError"));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #d9534f;"));
    error_->hide();
    column->addWidget(error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &TieDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    column->addWidget(buttons);
}

QString TieDialog::className() const { return class_->currentText().trimmed(); }
void TieDialog::setClassName(const QString& name) { class_->setCurrentText(name); }
QString TieDialog::error() const { return error_->isHidden() ? QString() : error_->text(); }

bool TieDialog::apply() {
    error_->hide();
    const auto name = className();
    const auto invalid = [&] {
        if (name.isEmpty()) return true;
        for (const QChar c : name) {
            if (c.isSpace() || c == QLatin1Char('"') || !c.isPrint()) return true;
        }
        return false;
    }();
    if (invalid) {
        error_->setText(tr("Enter a class name without spaces or quotes."));
        error_->show();
        return false;
    }
    editor::TieParams params;
    params.className = name.toStdString();
    if (auto done = session_.tieToEntity(params); !done) {
        error_->setText(QString::fromStdString(done.error().what()));
        error_->show();
        return false;
    }
    return true;
}

void TieDialog::accept() {
    if (apply()) QDialog::accept();
}

}  // namespace apex::ui
