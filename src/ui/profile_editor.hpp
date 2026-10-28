#pragma once

#include "compile/sequence.hpp"

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QTableWidget;

namespace apex::ui {

class ProfileEditor final : public QDialog {
    Q_OBJECT
public:
    explicit ProfileEditor(compile::CompileProfile profile, QWidget* parent = nullptr);
    // The edited profile; special commands keep their kind and only their arguments are editable.
    compile::CompileProfile profile() const;
private:
    void addRow(const compile::Command& command, int row = -1);
    void moveRow(int delta);
    compile::CompileProfile base_;
    QLineEdit* name_;
    QCheckBox* copyBsp_;
    QCheckBox* runGame_;
    QCheckBox* cordonOnly_;
    QLineEdit* cordonMaterial_;
    QTableWidget* commands_;
};

} // namespace apex::ui
