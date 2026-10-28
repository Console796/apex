#include "ui/profile_editor.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace apex::ui {
namespace {
enum Column { Enabled, Executable, Arguments, Wait };
constexpr int specialRole = Qt::UserRole + 1;
QString specialLabel(compile::SpecialCommand command) {
    switch (command) {
    case compile::SpecialCommand::ChangeDirectory: return QObject::tr("Change directory");
    case compile::SpecialCommand::Copy: return QObject::tr("Copy");
    case compile::SpecialCommand::Delete: return QObject::tr("Delete");
    case compile::SpecialCommand::Rename: return QObject::tr("Rename");
    case compile::SpecialCommand::CopyIfExists: return QObject::tr("Copy if exists");
    }
    return {};
}
QTableWidgetItem* checkItem(bool checked) {
    auto* item = new QTableWidgetItem;
    item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}
}

ProfileEditor::ProfileEditor(compile::CompileProfile profile, QWidget* parent)
    : QDialog(parent), base_(std::move(profile)) {
    setWindowTitle(tr("Edit compile profile"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    name_ = new QLineEdit(QString::fromStdString(base_.name), this);
    name_->setObjectName(QStringLiteral("profileName"));
    copyBsp_ = new QCheckBox(tr("Copy BSP to maps"), this);
    copyBsp_->setChecked(base_.copyBspToMaps);
    runGame_ = new QCheckBox(tr("Run game after"), this);
    runGame_->setObjectName(QStringLiteral("profileRunGame"));
    runGame_->setChecked(base_.runGameAfter);
    cordonOnly_ = new QCheckBox(tr("Compile the cordon only"), this);
    cordonOnly_->setChecked(base_.cordonOnly);
    form->addRow(tr("Name"), name_);
    form->addRow(copyBsp_);
    form->addRow(runGame_);
    form->addRow(cordonOnly_);
    cordonMaterial_ = new QLineEdit(QString::fromStdString(base_.cordonMaterial), this);
    cordonMaterial_->setObjectName(QStringLiteral("profileCordonMaterial"));
    cordonMaterial_->setToolTip(tr("Material of the six walls added around the cordon in the compiled copy"));
    form->addRow(tr("Cordon wall material"), cordonMaterial_);
    layout->addLayout(form);
    commands_ = new QTableWidget(0, 4, this);
    commands_->setObjectName(QStringLiteral("profileCommands"));
    commands_->setHorizontalHeaderLabels({tr("On"), tr("Executable"), tr("Arguments"), tr("Wait")});
    commands_->horizontalHeader()->setSectionResizeMode(Executable, QHeaderView::ResizeToContents);
    commands_->horizontalHeader()->setSectionResizeMode(Arguments, QHeaderView::Stretch);
    commands_->setSelectionBehavior(QAbstractItemView::SelectRows);
    commands_->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const auto& command : base_.commands) addRow(command);
    layout->addWidget(commands_);
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    auto* up = new QPushButton(tr("Up"), this);
    auto* down = new QPushButton(tr("Down"), this);
    add->setObjectName(QStringLiteral("profileAdd"));
    remove->setObjectName(QStringLiteral("profileRemove"));
    up->setObjectName(QStringLiteral("profileUp"));
    down->setObjectName(QStringLiteral("profileDown"));
    for (auto* button : {add, remove, up, down}) buttons->addWidget(button);
    buttons->addStretch();
    layout->addLayout(buttons);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(box);
    connect(add, &QPushButton::clicked, this, [this] {
        compile::Command command;
        addRow(command, commands_->currentRow() >= 0 ? commands_->currentRow() + 1 : -1);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (commands_->currentRow() >= 0) commands_->removeRow(commands_->currentRow());
    });
    connect(up, &QPushButton::clicked, this, [this] { moveRow(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveRow(1); });
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    resize(720, 480);
}

void ProfileEditor::addRow(const compile::Command& command, int row) {
    if (row < 0 || row > commands_->rowCount()) row = commands_->rowCount();
    commands_->insertRow(row);
    commands_->setItem(row, Enabled, checkItem(command.enabled));
    auto* executable = new QTableWidgetItem(command.special ? specialLabel(*command.special) :
        QString::fromStdString(command.executable));
    if (command.special) {
        executable->setData(specialRole, static_cast<int>(*command.special));
        executable->setFlags(executable->flags() & ~Qt::ItemIsEditable);
    }
    commands_->setItem(row, Executable, executable);
    commands_->setItem(row, Arguments, new QTableWidgetItem(QString::fromStdString(command.arguments)));
    commands_->setItem(row, Wait, checkItem(command.wait));
}

void ProfileEditor::moveRow(int delta) {
    const int row = commands_->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= commands_->rowCount()) return;
    for (int column = 0; column < commands_->columnCount(); ++column) {
        auto* a = commands_->takeItem(row, column);
        auto* b = commands_->takeItem(target, column);
        commands_->setItem(row, column, b);
        commands_->setItem(target, column, a);
    }
    commands_->setCurrentCell(target, Executable);
}

compile::CompileProfile ProfileEditor::profile() const {
    auto result = base_;
    result.name = name_->text().trimmed().toStdString();
    result.copyBspToMaps = copyBsp_->isChecked();
    result.runGameAfter = runGame_->isChecked();
    result.cordonOnly = cordonOnly_->isChecked();
    if (const auto material = cordonMaterial_->text().trimmed(); !material.isEmpty())
        result.cordonMaterial = material.toStdString();
    result.commands.clear();
    for (int row = 0; row < commands_->rowCount(); ++row) {
        compile::Command command;
        command.enabled = commands_->item(row, Enabled)->checkState() == Qt::Checked;
        command.wait = commands_->item(row, Wait)->checkState() == Qt::Checked;
        command.arguments = commands_->item(row, Arguments)->text().toStdString();
        const auto* executable = commands_->item(row, Executable);
        if (const auto special = executable->data(specialRole); special.isValid())
            command.special = static_cast<compile::SpecialCommand>(special.toInt());
        else
            command.executable = executable->text().toStdString();
        result.commands.push_back(std::move(command));
    }
    return result;
}

} // namespace apex::ui
