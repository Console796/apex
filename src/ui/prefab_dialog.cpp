#include "ui/prefab_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include "editor/prefab.hpp"

namespace apex::ui {

PrefabDialog::PrefabDialog(editor::EditorSession& session, std::filesystem::path folder, QStringList categories,
                           QWidget* parent)
    : QDialog(parent), session_(session), folder_(std::move(folder)) {
    setObjectName(QStringLiteral("prefabDialog"));
    setWindowTitle(tr("Create Prefab from Selection"));
    auto* column = new QVBoxLayout(this);
    auto* where = new QLabel(tr("Saved in %1").arg(QString::fromStdString(folder_.string())));
    where->setObjectName(QStringLiteral("prefabFolder"));
    where->setWordWrap(true);
    column->addWidget(where);
    auto* form = new QFormLayout;
    name_ = new QLineEdit;
    name_->setObjectName(QStringLiteral("prefabName"));
    form->addRow(tr("Name"), name_);
    category_ = new QComboBox;
    category_->setObjectName(QStringLiteral("prefabCategory"));
    category_->setEditable(true);
    category_->setInsertPolicy(QComboBox::NoInsert);
    categories.removeAll(QString());
    categories.removeDuplicates();
    category_->addItem(QString());
    category_->addItems(categories);
    category_->setCurrentIndex(0);
    category_->lineEdit()->setPlaceholderText(tr("(none)"));
    form->addRow(tr("Category"), category_);
    column->addLayout(form);
    overwrite_ = new QCheckBox(tr("Replace an existing prefab of this name"));
    overwrite_->setObjectName(QStringLiteral("prefabOverwrite"));
    column->addWidget(overwrite_);
    error_ = new QLabel;
    error_->setObjectName(QStringLiteral("prefabError"));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #d9534f;"));
    error_->hide();
    column->addWidget(error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &PrefabDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    column->addWidget(buttons);
    name_->setFocus();
}

QString PrefabDialog::prefabName() const { return name_->text().trimmed(); }
QString PrefabDialog::category() const { return category_->currentText().trimmed(); }
void PrefabDialog::setPrefabName(const QString& name) { name_->setText(name); }
void PrefabDialog::setCategory(const QString& category) { category_->setCurrentText(category); }
void PrefabDialog::setOverwrite(bool overwrite) { overwrite_->setChecked(overwrite); }
QString PrefabDialog::error() const { return error_->isHidden() ? QString() : error_->text(); }

bool PrefabDialog::create() {
    error_->hide();
    if (prefabName().isEmpty()) {
        error_->setText(tr("Enter a name for the prefab."));
        error_->show();
        return false;
    }
    const auto file = editor::createPrefab(session_, prefabName().toStdString(), category().toStdString(), folder_,
                                           overwrite_->isChecked());
    if (!file) {
        error_->setText(QString::fromStdString(file.error().what()));
        error_->show();
        return false;
    }
    created_ = *file;
    emit created(QString::fromStdString(file->string()));
    return true;
}

void PrefabDialog::accept() {
    if (create()) QDialog::accept();
}

}  // namespace apex::ui
