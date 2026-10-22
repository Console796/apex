#pragma once

#include <QDialog>
#include <QStringList>

#include "editor/editor_session.hpp"

class QComboBox;
class QLabel;

namespace apex::ui {

// Tie to entity (plan 0.2.6): choose the brush-entity class for the selected solids. The class
// list comes from the loaded FGD's solid classes when there is one; the box also takes free text.
class TieDialog final : public QDialog {
    Q_OBJECT
public:
    TieDialog(editor::EditorSession& session, QStringList classes, QWidget* parent = nullptr);

    QString className() const;
    void setClassName(const QString& name);
    bool apply();
    QString error() const;

public slots:
    void accept() override;

private:
    editor::EditorSession& session_;
    QComboBox* class_ = nullptr;
    QLabel* error_ = nullptr;
};

}  // namespace apex::ui
