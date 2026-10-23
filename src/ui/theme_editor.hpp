#pragma once

#include <QDialog>
#include <QMap>

#include "ui/theme.hpp"

class QLineEdit;
class QPushButton;
class QSpinBox;

namespace apex::ui {

class MainWindow;

// Edits one custom theme: a name, the colour tokens and the main metrics. Every change previews
// live on the whole application; Cancel restores the theme that was active on entry, Save writes
// the theme file and activates it.
class ThemeEditorDialog : public QDialog {
    Q_OBJECT
public:
    // `initial.id` is the custom theme being edited, or empty for a new one.
    ThemeEditorDialog(MainWindow& window, const ThemeEntry& initial, QWidget* parent = nullptr);

    // The id of the saved theme once the dialog was accepted.
    QString savedId() const { return savedId_; }

private:
    void setColour(const QString& name, const QColor& colour);
    void chooseColour(const QString& name);
    void preview();
    void save();
    void reject() override;

    MainWindow& window_;
    ThemeEntry theme_;
    QString previousId_;
    QString savedId_;
    QLineEdit* name_ = nullptr;
    QMap<QString, QLineEdit*> hex_;
    QMap<QString, QPushButton*> swatch_;
    QSpinBox* spacing_ = nullptr;
    QSpinBox* radius_ = nullptr;
    QSpinBox* controlHeight_ = nullptr;
    QSpinBox* fontSize_ = nullptr;
};

}  // namespace apex::ui
