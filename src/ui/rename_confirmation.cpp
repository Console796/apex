#include "ui/rename_confirmation.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace apex::ui {

bool renameWithConfirmation(editor::EditorSession& session, document::NodeId entity,
                            std::string name, QWidget* parent,
                            const RenameDialogPresenter& presenter,
                            const std::function<bool()>& stillValid) {
    const auto plan = session.planEntityRename(entity, name);
    if (plan.changes.empty()) return static_cast<bool>(session.renameEntity(entity, name, true));

    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("renameReferencesDialog"));
    dialog.setWindowTitle(QObject::tr("Update references?"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* changes = new QPlainTextEdit(&dialog);
    changes->setObjectName(QStringLiteral("renameReferenceList"));
    changes->setReadOnly(true);
    std::string lines;
    for (const auto& line : editor::describe(plan)) lines += line + '\n';
    changes->setPlainText(QString::fromStdString(lines));
    layout->addWidget(changes);
    auto* update = new QCheckBox(QObject::tr("Update %1 references").arg(plan.changes.size()), &dialog);
    update->setObjectName(QStringLiteral("renameUpdateReferences"));
    update->setChecked(true);
    layout->addWidget(update);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const bool accepted = presenter ? presenter(&dialog) : dialog.exec() == QDialog::Accepted;
    if (stillValid && !stillValid()) return false;
    return accepted && static_cast<bool>(session.renameEntity(entity, name, update->isChecked()));
}

} // namespace apex::ui
