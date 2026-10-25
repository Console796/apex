#include "ui/inspector_panel.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleValidator>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>

namespace apex::ui {
namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
std::atomic<std::uint64_t> nextFocus{1};
QString token() { return QString::number(nextFocus++); }
}

InspectorPanel::InspectorPanel(QWidget* parent) : QWidget(parent) {
    layout_ = new QVBoxLayout(this);
    auto* header = new QHBoxLayout;
    classPicker_ = new QComboBox(this);
    classPicker_->setObjectName(QStringLiteral("inspectorClass"));
    classPicker_->setEditable(true);
    classPicker_->setInsertPolicy(QComboBox::NoInsert);
    classPicker_->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    raw_ = new QCheckBox(tr("Raw"), this);
    raw_->setObjectName(QStringLiteral("inspectorRaw"));
    header->addWidget(classPicker_, 1);
    header->addWidget(raw_);
    layout_->addLayout(header);
    connect(raw_, &QCheckBox::toggled, this, [this] { requestRefresh(); });
    connect(classPicker_, &QComboBox::activated, this, [this] {
        if (!rebuilding_ && current(entities_)) session_->setClass(classPicker_->currentText().toStdString());
    });
    refresh();
}
InspectorPanel::~InspectorPanel() {
    setEditorSession(nullptr);
    setDocument(nullptr);
}
void InspectorPanel::setDocument(document::Document* document) {
    if (document_) document_->unobserve(documentObserver_);
    document_ = document;
    if (document_) documentObserver_ = document_->observe([this](const document::ChangeSet&) { requestRefresh(); });
    refresh();
}
void InspectorPanel::setEditorSession(editor::EditorSession* session) {
    if (session_) session_->unobserve(sessionObserver_);
    session_ = session;
    if (session_) {
        selectionRevision_ = session_->selection().changed();
        classesRevision_ = session_->entityClassesRevision();
        sessionObserver_ = session_->observe([this] {
            if (selectionRevision_ != session_->selection().changed() ||
                classesRevision_ != session_->entityClassesRevision() || session_->document() != document_) {
                selectionRevision_ = session_->selection().changed();
                classesRevision_ = session_->entityClassesRevision();
                requestRefresh();
            }
        });
    }
    refresh();
}
void InspectorPanel::setMaterialBrowser(std::function<void(QString, QString, std::function<void(QString)>)> browser) {
    materialBrowser_ = std::move(browser);
    requestRefresh();
}
bool InspectorPanel::current(const std::vector<document::NodeId>& entities) const {
    return session_ && document_ && session_->document() == document_ && !entities.empty() &&
           session_->inspectorModel().entities() == entities;
}
void InspectorPanel::requestRefresh() {
    if (queued_) return;
    queued_ = true;
    QTimer::singleShot(0, this, [this] { queued_ = false; refresh(); });
}
void InspectorPanel::watch(QWidget* field) {
    field->setProperty("inspectorFocus", token());
    field->installEventFilter(this);
}
bool InspectorPanel::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::FocusIn) object->setProperty("inspectorFocus", token());
    if (event->type() == QEvent::FocusOut) requestRefresh();
    return QWidget::eventFilter(object, event);
}
void InspectorPanel::refresh() {
    if (rebuilding_) return;
    const bool attached = session_ && document_ && session_->document() == document_;
    auto* focus = QApplication::focusWidget();
    if (attached && current(entities_) && focus && rows_ && rows_->isAncestorOf(focus) &&
        rows_->property("raw").toBool() == raw_->isChecked()) return;
    rebuilding_ = true;
    const QSignalBlocker classBlock(classPicker_);
    classPicker_->clear();
    entities_.clear();
    if (attached) {
        for (const auto& [key, cls] : session_->entityClasses().classes) {
            if (!ciEquals(cls.type, "BaseClass")) classPicker_->addItem(text(cls.name));
        }
        entities_ = session_->inspectorModel().entities();
    }
    classPicker_->setEnabled(!entities_.empty());
    raw_->setEnabled(!entities_.empty());
    if (rows_) { layout_->removeWidget(rows_); delete rows_; }
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    rows_ = scroll;
    rows_->setProperty("raw", raw_->isChecked());
    layout_->addWidget(scroll);
    auto* content = new QWidget(scroll);
    auto* form = new QFormLayout(content);
    scroll->setWidget(content);
    if (entities_.empty()) {
        classPicker_->setCurrentIndex(-1);
        form->addRow(new QLabel(tr("Select an entity to inspect."), content));
        rebuilding_ = false;
        return;
    }
    const auto model = session_->inspectorModel(raw_->isChecked());
    const auto entities = entities_;
    QString classname;
    bool mixedClass = false;
    for (const auto id : entities) {
        const auto* key = document_->get(*document_->handle(id))->block->find("classname");
        const auto name = key ? text(key->decodedValue().value_or("")) : QString{};
        if (id == entities.front()) classname = name;
        else mixedClass |= classname != name;
    }
    classPicker_->setCurrentIndex(-1);
    classPicker_->setEditText(mixedClass ? QString{} : classname);
    classPicker_->lineEdit()->setPlaceholderText(mixedClass ? tr("(mixed)") : QString{});
    bool other = false;
    for (std::size_t index = 0; index < model.rows().size(); ++index) {
        const auto& row = model.rows()[index];
        if (!other && row.section == editor::InspectorModel::Section::Other) {
            form->addRow(new QLabel(tr("Other keys"), content));
            other = true;
        }
        auto* field = new QWidget(content);
        field->setObjectName(QStringLiteral("property.%1.%2").arg(text(row.key)).arg(row.occurrence));
        field->setEnabled(!row.readOnly);
        field->setToolTip(row.property ? text(row.property->description) : QString{});
        auto* horizontal = new QHBoxLayout(field);
        horizontal->setContentsMargins(0, 0, 0, 0);
        const auto type = row.property ? row.property->type : std::string{};
        const bool raw = raw_->isChecked();
        const auto valid = [this, entities, index, raw, key = row.key, occurrence = row.occurrence, type] {
            if (rebuilding_ || !current(entities) || raw_->isChecked() != raw) return false;
            const auto latest = session_->inspectorModel(raw);
            if (index >= latest.rows().size()) return false;
            const auto& latestRow = latest.rows()[index];
            return latestRow.key == key && latestRow.occurrence == occurrence && !latestRow.readOnly &&
                   (latestRow.property ? latestRow.property->type : std::string{}) == type;
        };
        const auto write = [this, valid, index, raw](const QString& value, const QString& focusToken) {
            if (valid())
                session_->editProperty(index, value.toStdString(), focusToken.toStdString(), raw);
        };
        if (type == "choices") {
            auto* combo = new QComboBox(field);
            combo->setObjectName(QStringLiteral("choice.%1").arg(text(row.key)));
            for (const auto& choice : row.property->choices) combo->addItem(text(choice.label), text(choice.value));
            combo->setCurrentIndex(row.mixed ? -1 : combo->findData(text(row.value)));
            combo->setPlaceholderText(row.mixed ? tr("(mixed)") : text(row.value));
            watch(combo);
            horizontal->addWidget(combo);
            connect(combo, &QComboBox::activated, this, [this, valid, index, combo](int choice) {
                if (valid()) session_->setChoice(index, combo->itemData(choice).toString().toStdString(),
                                                  combo->property("inspectorFocus").toString().toStdString());
            });
        } else if (type == "flags") {
            auto* flags = new QVBoxLayout;
            horizontal->addLayout(flags);
            for (const auto& flag : row.property->flags) {
                auto* check = new QCheckBox(text(flag.label), field);
                check->setObjectName(QStringLiteral("flag.%1.%2").arg(text(row.key)).arg(flag.bit));
                bool first = true, enabled = false, mixed = false;
                for (const auto id : entities) {
                    const auto& block = document_->get(*document_->handle(id))->block;
                    const auto* key = block->find(row.key);
                    const auto* clsKey = block->find("classname");
                    const auto* cls = clsKey ? session_->entityClasses().find(clsKey->decodedValue().value_or("")) : nullptr;
                    std::string value = row.property->defaultValue;
                    if (cls) for (const auto& prop : cls->properties) if (ciEquals(prop.name, row.key)) value = prop.defaultValue;
                    if (key) value = key->decodedValue().value_or("");
                    const bool bit = (text(value).toULongLong() & flag.bit) != 0;
                    if (first) enabled = bit;
                    else mixed |= enabled != bit;
                    first = false;
                }
                check->setTristate(mixed);
                check->setCheckState(mixed ? Qt::PartiallyChecked : enabled ? Qt::Checked : Qt::Unchecked);
                watch(check);
                flags->addWidget(check);
                connect(check, &QCheckBox::clicked, this, [this, valid, index, bit = flag.bit, check](bool checked) {
                    check->setTristate(false);
                    if (valid()) session_->toggleFlag(index, bit, checked,
                                                    check->property("inspectorFocus").toString().toStdString());
                });
            }
        } else if (type == "color255") {
            auto* button = new QPushButton(row.mixed ? tr("(mixed)") : text(row.value), field);
            watch(button);
            horizontal->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, button = QPointer<QPushButton>(button), write, value = row.value] {
                const auto parts = text(value).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                const QColor initial = parts.size() == 3 ? QColor(parts[0].toInt(), parts[1].toInt(), parts[2].toInt()) : QColor(Qt::white);
                const auto color = QColorDialog::getColor(initial, this, tr("Entity colour"));
                if (color.isValid()) {
                    const auto valueText = QStringLiteral("%1 %2 %3").arg(color.red()).arg(color.green()).arg(color.blue());
                    write(valueText, token());
                    if (button) button->setText(valueText);
                }
            });
        } else {
            const bool vector = type == "origin" || type == "angles" || type == "angle" || row.key == "origin" || row.key == "angles";
            const int count = vector && !raw ? 3 : 1;
            const auto parts = text(row.value).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            QList<QLineEdit*> edits;
            for (int component = 0; component < count; ++component) {
                auto* edit = new QLineEdit(field);
                edit->setObjectName(count == 1 ? QStringLiteral("edit.%1.%2").arg(text(row.key)).arg(row.occurrence)
                                              : QStringLiteral("edit.%1.%2").arg(text(row.key)).arg(component));
                edit->setText(row.mixed ? QString{} : count == 1 ? text(row.value) : parts.value(component));
                edit->setPlaceholderText(row.mixed ? tr("(mixed)") : QString{});
                if (type == "integer") edit->setValidator(new QIntValidator(edit));
                else if (type == "float" || count == 3) edit->setValidator(new QDoubleValidator(edit));
                QStringList candidates;
                for (const auto& candidate : model.autocomplete(index)) candidates.push_back(text(candidate));
                if (!candidates.empty()) {
                    auto* completer = new QCompleter(candidates, edit);
                    completer->setCaseSensitivity(Qt::CaseInsensitive);
                    edit->setCompleter(completer);
                }
                watch(edit);
                horizontal->addWidget(edit);
                edits.push_back(edit);
            }
            for (int component = 0; component < edits.size(); ++component) {
                auto* edit = edits[component];
                connect(edit, &QLineEdit::textEdited, this, [this, write, valid, index, component, edit, count](const QString& value) {
                    if (!edit->hasAcceptableInput() || (count == 3 && value.isEmpty())) return;
                    const auto focusKey = edit->property("inspectorFocus").toString();
                    if (count == 1) write(value, focusKey);
                    else if (valid()) session_->editVectorComponent(index, static_cast<std::size_t>(component),
                                                                   value.toStdString(), focusKey.toStdString());
                });
            }
            if (type == "studio" || type == "material" || type == "sound") {
                auto* browse = new QPushButton(tr("Browse…"), field);
                browse->setObjectName(QStringLiteral("inspectorBrowse.%1").arg(text(row.key)));
                browse->setEnabled(type == "material" && static_cast<bool>(materialBrowser_));
                horizontal->addWidget(browse);
                if (type == "material") connect(browse, &QPushButton::clicked, this,
                    [this, valid, write, currentValue = text(row.value), what = text(row.key)] {
                        if (!materialBrowser_ || !valid()) return;
                        QPointer<InspectorPanel> self(this);
                        materialBrowser_(currentValue, what, [self, write](QString value) {
                            if (self) write(value, token());
                        });
                    });
            }
        }
        form->addRow(row.property && !row.property->displayName.empty() ? text(row.property->displayName) : text(row.key), field);
    }
    rebuilding_ = false;
}

} // namespace apex::ui
