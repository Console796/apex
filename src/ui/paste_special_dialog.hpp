#pragma once

#include <QDialog>
#include <QString>

#include "editor/editor_session.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace apex::ui {

// Paste Special (plan 0.2.6, D12): copies, per-copy offset and rotation, grouping and unique
// entity names for one clipboard fragment. Fields are typed and parsed strictly; any invalid
// field shows an error and leaves the document untouched. A successful Apply is one undo step.
class PasteSpecialDialog final : public QDialog {
    Q_OBJECT
public:
    // `text` is the VMF fragment; `base` carries the placement (in place, or position and plane).
    PasteSpecialDialog(editor::EditorSession& session, std::string text, editor::PasteOptions base,
                       QWidget* parent = nullptr);

    bool apply();
    QString error() const;

public slots:
    void accept() override;

private:
    QLineEdit* field(const QString& name, const QString& initial);
    void showError(const QString& text);

    editor::EditorSession& session_;
    std::string text_;
    editor::PasteOptions base_;
    QLineEdit *copies_ = nullptr, *ox_ = nullptr, *oy_ = nullptr, *oz_ = nullptr;
    QLineEdit *pitch_ = nullptr, *yaw_ = nullptr, *roll_ = nullptr, *affix_ = nullptr;
    QCheckBox *group_ = nullptr, *rewrite_ = nullptr;
    QComboBox* names_ = nullptr;
    QLabel* error_ = nullptr;
};

}  // namespace apex::ui
