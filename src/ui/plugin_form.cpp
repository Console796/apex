#include "ui/plugin_form.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <set>

namespace apex::ui {
namespace {
QString text(apex_str s) { return s.data ? QString::fromUtf8(s.data, static_cast<qsizetype>(s.size)) : QString(); }
}
// A plugin may drop a control from inside that control's own signal, so it goes away on the next turn.
void PluginForm::retire(QWidget* widget) {
    layout_->removeWidget(widget);
    widget->setObjectName({});
    widget->hide();
    widget->deleteLater();
}
PluginForm::PluginForm(QWidget* parent) : QWidget(parent), layout_(new QVBoxLayout(this)) {
    layout_->setContentsMargins(0, 0, 0, 0);
}
bool PluginForm::describe(const apex_ui_form& form) {
    if (form.struct_size < sizeof(form) || (form.control_count && !form.controls)) return false;
    std::set<QString> keys;
    for (size_t i = 0; i < form.control_count; ++i) {
        const auto& c = form.controls[i];
        if (c.struct_size < sizeof(c) || c.value.struct_size < sizeof(c.value) || text(c.key).isEmpty() ||
            c.kind > APEX_UI_SECTION ||
            (c.kind != APEX_UI_SECTION && c.child_count) || (c.child_count && !c.children) ||
            !keys.insert(text(c.key)).second) return false;
    }
    for (auto* widget : findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!keys.contains(widget->objectName())) retire(widget);
    }
    for (size_t i = 0; i < form.control_count; ++i) {
        const auto& c = form.controls[i];
        const auto key = text(c.key);
        auto* widget = findChild<QWidget*>(key, Qt::FindDirectChildrenOnly);
        if (widget && widget->property("apexKind").toUInt() != c.kind) {
            retire(widget); widget = nullptr;
        }
        if (!widget) {
            switch (c.kind) {
            case APEX_UI_LABEL: widget = new QLabel(this); break;
            case APEX_UI_TEXT: widget = new QLineEdit(this); break;
            case APEX_UI_NUMBER: widget = new QDoubleSpinBox(this); break;
            case APEX_UI_CHECKBOX: widget = new QCheckBox(this); break;
            case APEX_UI_CHOICE: widget = new QComboBox(this); break;
            case APEX_UI_BUTTON: widget = new QPushButton(this); break;
            case APEX_UI_COLOUR: widget = new QPushButton(this); break;
            case APEX_UI_TABLE: widget = new QListWidget(this); break;
            case APEX_UI_MATERIAL: case APEX_UI_MODEL: case APEX_UI_SOUND: {
                auto* picker = new QComboBox(this); picker->setEditable(true); widget = picker; break;
            }
            case APEX_UI_SECTION: {
                auto* group = new QGroupBox(this);
                group->setCheckable(true); group->setChecked(true);
                auto* layout = new QVBoxLayout(group);
                layout->addWidget(new PluginForm(group));
                widget = group;
                break;
            }
            default: return false;
            }
            widget->setObjectName(key); widget->setProperty("apexKind", c.kind);
            const auto sendText = [this, key](const QString& value) {
                if (!change_) return;
                const auto k = key.toUtf8(), v = value.toUtf8();
                const apex_ui_value result{sizeof(result), {v.constData(), static_cast<size_t>(v.size())}, 0, 0};
                change_(std::string_view(k.constData(), static_cast<size_t>(k.size())), result);
            };
            if (auto* edit = qobject_cast<QLineEdit*>(widget)) connect(edit, &QLineEdit::textEdited, this, sendText);
            if (auto* number = qobject_cast<QDoubleSpinBox*>(widget)) connect(number, &QDoubleSpinBox::valueChanged, this, [this, key](double n) {
                if (!change_) return;
                const auto k = key.toUtf8();
                const apex_ui_value v{sizeof(v), {}, n, 0}; change_(std::string_view(k.constData(), static_cast<size_t>(k.size())), v);
            });
            if (auto* check = qobject_cast<QCheckBox*>(widget)) connect(check, &QCheckBox::toggled, this, [this, key](bool checked) {
                if (!change_) return;
                const auto k = key.toUtf8();
                const apex_ui_value v{sizeof(v), {}, 0, checked ? 1u : 0u}; change_(std::string_view(k.constData(), static_cast<size_t>(k.size())), v);
            });
            if (auto* choice = qobject_cast<QComboBox*>(widget)) connect(choice, &QComboBox::currentTextChanged, this, sendText);
            if (auto* list = qobject_cast<QListWidget*>(widget)) connect(list, &QListWidget::currentTextChanged, this, sendText);
            if (auto* button = qobject_cast<QPushButton*>(widget); button && c.kind == APEX_UI_BUTTON) connect(button, &QPushButton::clicked, this, [this, key] {
                if (!click_) return;
                const auto k = key.toUtf8(); click_(std::string_view(k.constData(), static_cast<size_t>(k.size())));
            });
            if (auto* button = qobject_cast<QPushButton*>(widget); button && c.kind == APEX_UI_COLOUR)
                connect(button, &QPushButton::clicked, this, [this, button, sendText] {
                    const auto chosen = QColorDialog::getColor(QColor(button->text()), this);
                    if (chosen.isValid()) { button->setText(chosen.name(QColor::HexArgb)); sendText(button->text()); }
                });
        }
        const QSignalBlocker block(widget);
        widget->setToolTip(text(c.label));
        if (auto* label = qobject_cast<QLabel*>(widget)) label->setText(text(c.value.text));
        if (auto* edit = qobject_cast<QLineEdit*>(widget)) { if (edit->text() != text(c.value.text)) edit->setText(text(c.value.text)); }
        if (auto* number = qobject_cast<QDoubleSpinBox*>(widget)) { number->setRange(c.minimum, c.maximum); number->setValue(c.value.number); }
        if (auto* check = qobject_cast<QCheckBox*>(widget)) { check->setText(text(c.label)); check->setChecked(c.value.checked != 0); }
        if (auto* choice = qobject_cast<QComboBox*>(widget)) {
            QStringList items;
            if (c.options) for (size_t j = 0; j < c.option_count; ++j) items << text(c.options[j]);
            QStringList current;
            for (int j = 0; j < choice->count(); ++j) current << choice->itemText(j);
            if (items != current) { choice->clear(); choice->addItems(items); }
            if (choice->currentText() != text(c.value.text)) choice->setCurrentText(text(c.value.text));
        }
        if (auto* list = qobject_cast<QListWidget*>(widget)) {
            list->clear(); if (c.options) for (size_t j = 0; j < c.option_count; ++j) list->addItem(text(c.options[j]));
            const auto matches = list->findItems(text(c.value.text), Qt::MatchExactly);
            if (!matches.empty()) list->setCurrentItem(matches.front());
        }
        if (auto* button = qobject_cast<QPushButton*>(widget)) button->setText(c.kind == APEX_UI_COLOUR ? text(c.value.text) : text(c.label));
        if (auto* group = qobject_cast<QGroupBox*>(widget)) {
            group->setTitle(text(c.label));
            auto* child = static_cast<PluginForm*>(group->layout()->itemAt(0)->widget());
            child->setChange([this](std::string_view childKey, const apex_ui_value& value) { if (change_) change_(childKey, value); });
            child->setClick([this](std::string_view childKey) { if (click_) click_(childKey); });
            const apex_ui_form nested{sizeof(nested), c.children, c.child_count};
            if (!child->describe(nested)) return false;
        }
        layout_->insertWidget(static_cast<int>(i), widget);
    }
    return true;
}
}
