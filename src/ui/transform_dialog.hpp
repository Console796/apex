#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include "editor/editor_session.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QTabWidget;

namespace apex::ui {

// Ctrl+M: exact Move / Rotate / Scale values for the selection (plan 0.2.5). Values are typed,
// not spun, so malformed input can be rejected as a whole: an error is shown and the document is
// left untouched. Each successful Apply is one undo step through the session.
class TransformDialog final : public QDialog {
    Q_OBJECT
public:
    enum Tab { Move = 0, Rotate = 1, Scale = 2 };

    TransformDialog(editor::EditorSession& session, QWidget* parent = nullptr);

    void setTab(Tab tab);
    Tab tab() const;
    // Applies the current tab. Returns false (with error() set) when a field is invalid, nothing
    // is selected, or the transform is degenerate.
    bool apply();
    QString error() const;

public slots:
    void accept() override;

private:
    struct Fields {
        QLineEdit* x = nullptr;
        QLineEdit* y = nullptr;
        QLineEdit* z = nullptr;
    };
    QWidget* page(const QString& prefix, const QStringList& names, const QString& xLabel, const QString& yLabel, const QString& zLabel,
                  const QString& initial, Fields& fields, QCheckBox** relative);
    void fillMove();
    void showError(const QString& text);

    editor::EditorSession& session_;
    QTabWidget* tabs_ = nullptr;
    Fields move_, rotate_, scale_;
    QCheckBox* relative_ = nullptr;
    QLabel* error_ = nullptr;
};

}  // namespace apex::ui
