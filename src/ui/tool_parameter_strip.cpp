#include "ui/tool_parameter_strip.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>

namespace apex::ui {
ToolParameterStrip::ToolParameterStrip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ToolParameterStrip"));
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    scroll->setFixedHeight(fontMetrics().height() + 38);
    auto* content = new QWidget(scroll);
    row_ = new QHBoxLayout(content);
    row_->setContentsMargins(4, 2, 4, 2);
    row_->setSpacing(4);
    row_->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(content);
    outer->addWidget(scroll);
    hide();
}
void ToolParameterStrip::update(editor::Tool& tool) {
    const auto parameters = tool.readout().parameters;
    bool same = tool_ == &tool && schema_.size() == parameters.size();
    if (same) for (std::size_t i = 0; i < parameters.size(); ++i) {
        const auto& a = schema_[i];
        const auto& b = parameters[i];
        same &= a.id == b.id && a.kind == b.kind && a.minimum == b.minimum &&
                a.maximum == b.maximum && a.choices == b.choices;
    }
    tool_ = &tool;
    if (!same) {
        while (auto* item = row_->takeAt(0)) {
            if (auto* widget = item->widget()) { widget->hide(); widget->deleteLater(); }
            delete item;
        }
        fields_.clear();
        schema_ = parameters;
        for (const auto& parameter : parameters) {
            row_->addWidget(new QLabel(QString::fromStdString(parameter.label), this));
            QWidget* field;
            if (parameter.kind == editor::ToolParameter::Kind::Choice) {
                auto* combo = new QComboBox(this);
                for (const auto& choice : parameter.choices) combo->addItem(QString::fromStdString(choice));
                connect(combo, &QComboBox::currentIndexChanged, this, [this, id = parameter.id](int value) {
                    tool_->setParameter(id, value);
                    update(*tool_);
                });
                field = combo;
            } else {
                auto* spin = new QDoubleSpinBox(this);
                spin->setDecimals(parameter.kind == editor::ToolParameter::Kind::Integer ? 0 : 2);
                spin->setRange(parameter.minimum, parameter.maximum);
                spin->setKeyboardTracking(false);
                connect(spin, &QDoubleSpinBox::valueChanged, this, [this, id = parameter.id](double value) {
                    tool_->setParameter(id, value);
                    update(*tool_);
                });
                field = spin;
            }
            field->setObjectName(QStringLiteral("toolParameter_") + QString::fromStdString(parameter.id));
            row_->addWidget(field);
            fields_[parameter.id] = field;
        }
        row_->addStretch();
    }
    for (const auto& parameter : parameters) {
        auto* field = fields_.at(parameter.id);
        const QSignalBlocker blocker(field);
        if (auto* combo = qobject_cast<QComboBox*>(field)) combo->setCurrentIndex(static_cast<int>(parameter.value));
        else if (auto* spin = qobject_cast<QDoubleSpinBox*>(field)) spin->setValue(parameter.value);
    }
    setVisible(!parameters.empty());
}
} // namespace apex::ui
