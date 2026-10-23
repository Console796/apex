#include "ui/theme_editor.hpp"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include "ui/main_window.hpp"

namespace apex::ui {
namespace {
void paintSwatch(QPushButton* b, const QColor& c) {
    b->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: 1px solid #808080; min-width: 40px; }").arg(c.name()));
}
}  // namespace

ThemeEditorDialog::ThemeEditorDialog(MainWindow& window, const ThemeEntry& initial, QWidget* parent)
    : QDialog(parent), window_(window), theme_(initial), previousId_(currentThemeId()) {
    setWindowTitle(tr("Edit theme"));
    setObjectName(QStringLiteral("themeEditor"));
    auto* root = new QVBoxLayout(this);

    name_ = new QLineEdit(theme_.name);
    name_->setObjectName(QStringLiteral("themeName"));
    auto* top = new QFormLayout;
    top->addRow(tr("Name"), name_);
    root->addLayout(top);

    auto* inner = new QWidget;
    auto* form = new QFormLayout(inner);
    for (const QString& n : colourTokenNames()) {
        const QColor c = colourToken(theme_.tokens, n);
        auto* edit = new QLineEdit(c.name());
        edit->setObjectName(QStringLiteral("themeHex_%1").arg(n));
        edit->setMaximumWidth(90);
        auto* sw = new QPushButton;
        sw->setObjectName(QStringLiteral("themeSwatch_%1").arg(n));
        paintSwatch(sw, c);
        hex_.insert(n, edit);
        swatch_.insert(n, sw);
        auto* row = new QHBoxLayout;
        row->addWidget(sw);
        row->addWidget(edit);
        row->addStretch(1);
        form->addRow(n, row);
        connect(sw, &QPushButton::clicked, this, [this, n] { chooseColour(n); });
        connect(edit, &QLineEdit::editingFinished, this, [this, n, edit] {
            const QColor parsed(edit->text().trimmed());
            if (parsed.isValid()) setColour(n, parsed);
            else edit->setText(colourToken(theme_.tokens, n).name());
        });
    }
    const auto spin = [&](const QString& label, int value, int lo, int hi) {
        auto* s = new QSpinBox;
        s->setRange(lo, hi);
        s->setValue(value);
        form->addRow(label, s);
        connect(s, &QSpinBox::valueChanged, this, [this] { preview(); });
        return s;
    };
    spacing_ = spin(tr("Spacing"), theme_.tokens.spacing, 0, 24);
    radius_ = spin(tr("Corner radius"), theme_.tokens.radius, 0, 16);
    controlHeight_ = spin(tr("Control height"), theme_.tokens.controlHeight, 16, 48);
    fontSize_ = spin(tr("Font size (pt)"), theme_.tokens.fontPointSize, 6, 24);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(inner);
    root->addWidget(scroll, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &ThemeEditorDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &ThemeEditorDialog::reject);
    root->addWidget(buttons);
    resize(380, 560);
    preview();
}

void ThemeEditorDialog::setColour(const QString& name, const QColor& colour) {
    colourToken(theme_.tokens, name) = colour;
    hex_[name]->setText(colour.name());
    paintSwatch(swatch_[name], colour);
    preview();
}

void ThemeEditorDialog::chooseColour(const QString& name) {
    const QColor c = QColorDialog::getColor(colourToken(theme_.tokens, name), this, name);
    if (c.isValid()) setColour(name, c);
}

void ThemeEditorDialog::preview() {
    theme_.tokens.spacing = spacing_->value();
    theme_.tokens.radius = radius_->value();
    theme_.tokens.controlHeight = controlHeight_->value();
    theme_.tokens.fontPointSize = fontSize_->value();
    window_.previewTokens(theme_.tokens);
}

void ThemeEditorDialog::save() {
    theme_.name = name_->text().trimmed();
    if (theme_.name.isEmpty()) {
        QMessageBox::warning(this, tr("Edit theme"), tr("Give the theme a name."));
        return;
    }
    theme_.builtin = false;
    QString error;
    const QString id = saveCustomTheme(theme_, themesDir(), &error);
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("Edit theme"), tr("Cannot save the theme: %1").arg(error));
        return;
    }
    savedId_ = id;
    window_.setThemeId(id);
    QDialog::accept();
}

void ThemeEditorDialog::reject() {
    window_.setThemeId(previousId_);
    QDialog::reject();
}

}  // namespace apex::ui
