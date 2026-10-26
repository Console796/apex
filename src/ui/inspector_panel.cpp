#include "ui/inspector_panel.hpp"
#include "editor/connection_completion.hpp"
#include "editor/incoming_connections.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleValidator>
#include <QEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QDialog>
#include <QPlainTextEdit>
#include <QKeyEvent>
#include <QMenu>
#include <QClipboard>
#include <QGuiApplication>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>

namespace apex::ui {
namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
std::atomic<std::uint64_t> nextFocus{1};
QString token() { return QString::number(nextFocus++); }

class ConnectionTable : public QTableWidget {
public:
    using QTableWidget::QTableWidget;
    std::function<void(QKeyEvent*)> keyHandler;
protected:
    void keyPressEvent(QKeyEvent* event) override {
        event->ignore();
        if (keyHandler) keyHandler(event);
        if (!event->isAccepted()) QTableWidget::keyPressEvent(event);
    }
};

class ConnectionDelegate : public QStyledItemDelegate {
public:
    using Candidates = std::function<QStringList(int, int)>;
    ConnectionDelegate(Candidates candidates, QObject* parent) : QStyledItemDelegate(parent), candidates_(std::move(candidates)) {}
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override {
        auto* editor = new QLineEdit(parent);
        auto* completer = new QCompleter(candidates_(index.row(), index.column()), editor);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchStartsWith);
        completer->setCompletionMode(QCompleter::PopupCompletion);
        editor->setCompleter(completer);
        return editor;
    }
private:
    Candidates candidates_;
};

enum OutputColumn { OutputName, TargetColumn, InputColumn, ParameterColumn, DelayColumn, TimesColumn };

// P2CE's FGD pads sections with readonly string properties whose label is a run of dashes.
bool isSpacerLabel(const QString& label) {
    const QString trimmed = label.trimmed();
    if (trimmed.size() < 3) return false;
    return std::all_of(trimmed.begin(), trimmed.end(), [](QChar c) { return c == u'-' || c == u'=' || c == u'_'; });
}

// The inspector sits in a scroll area the user scrolls often; a wheel over a combo must scroll the
// panel, never step the value.
class NoWheelComboBox : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

// A form label that shrinks to the space it is given, eliding the middle of long keys. The full text
// stays in the tooltip, so a long key never pushes the value column out of the inspector.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(const QString& full, QWidget* parent) : QLabel(parent), full_(full) {
        setToolTip(full);
        setMinimumWidth(40);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    }
    QSize sizeHint() const override {
        return {std::min(fontMetrics().horizontalAdvance(full_) + 2, kPreferredWidth), QLabel::sizeHint().height()};
    }
    QSize minimumSizeHint() const override { return {40, QLabel::minimumSizeHint().height()}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const QRect area = contentsRect();
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(area, static_cast<int>(alignment()),
                         fontMetrics().elidedText(full_, Qt::ElideMiddle, area.width()));
    }

private:
    static constexpr int kPreferredWidth = 190;
    QString full_;
};
}

InspectorPanel::InspectorPanel(QWidget* parent) : QWidget(parent) {
    layout_ = new QVBoxLayout(this);
    auto* header = new QHBoxLayout;
    classPicker_ = new NoWheelComboBox(this);
    classPicker_->setObjectName(QStringLiteral("inspectorClass"));
    classPicker_->setEditable(true);
    classPicker_->setInsertPolicy(QComboBox::NoInsert);
    classPicker_->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    raw_ = new QCheckBox(tr("Raw"), this);
    raw_->setObjectName(QStringLiteral("inspectorRaw"));
    header->addWidget(classPicker_, 1);
    header->addWidget(raw_);
    layout_->addLayout(header);
    tabs_ = new QTabWidget(this);
    properties_ = new QWidget(tabs_);
    propertiesLayout_ = new QVBoxLayout(properties_);
    propertiesLayout_->setContentsMargins(0, 0, 0, 0);
    outputs_ = new ConnectionTable(0, 6, tabs_);
    outputs_->setObjectName(QStringLiteral("inspectorOutputs"));
    outputs_->setHorizontalHeaderLabels({tr("Output"), tr("Target"), tr("Input"), tr("Parameter"), tr("Delay"), tr("Once")});
    outputs_->horizontalHeader()->setStretchLastSection(true);
    outputs_->setSelectionBehavior(QAbstractItemView::SelectRows);
    outputs_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    outputs_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    outputs_->horizontalHeaderItem(TimesColumn)->setToolTip(tr("Checked means times = 1 (fire once). Other values, including -1, are shown as numbers."));
    inputs_ = new QTableWidget(0, 5, tabs_);
    inputs_->setObjectName(QStringLiteral("inspectorInputs"));
    inputs_->setHorizontalHeaderLabels({tr("Source entity"), tr("Output"), tr("Input"), tr("Parameter"), tr("Delay")});
    inputs_->horizontalHeader()->setStretchLastSection(true);
    inputs_->setSelectionBehavior(QAbstractItemView::SelectRows);
    inputs_->setSelectionMode(QAbstractItemView::SingleSelection);
    inputs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tabs_->addTab(properties_, tr("Properties"));
    auto* outputsPage = new QWidget(tabs_);
    outputsPage->setObjectName(QStringLiteral("inspectorOutputsPage"));
    auto* outputsLayout = new QVBoxLayout(outputsPage);
    outputsLayout->setContentsMargins(0, 0, 0, 0);
    auto* outputButtons = new QHBoxLayout;
    const auto makeButton = [&](const QString& name, const QString& label, const QString& tip, void (InspectorPanel::*action)()) {
        auto* button = new QPushButton(label, outputsPage);
        button->setObjectName(name);
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QPushButton::clicked, this, action);
        outputButtons->addWidget(button);
    };
    makeButton(QStringLiteral("outputAdd"), tr("Add"), tr("Add an output below the current row (Ctrl+N or Insert)"), &InspectorPanel::addConnectionRow);
    makeButton(QStringLiteral("outputDuplicate"), tr("Duplicate"), tr("Duplicate the current row (Ctrl+D)"), &InspectorPanel::duplicateConnectionRow);
    makeButton(QStringLiteral("outputDelete"), tr("Delete"), tr("Delete the selected rows (Delete)"), &InspectorPanel::deleteConnectionRows);
    outputButtons->addStretch(1);
    outputsLayout->addLayout(outputButtons);
    outputsLayout->addWidget(outputs_);
    outputs_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(outputs_, &QWidget::customContextMenuRequested, this, &InspectorPanel::showConnectionMenu);
    tabs_->addTab(outputsPage, tr("Outputs"));
    tabs_->addTab(inputs_, tr("Inputs"));
    layout_->addWidget(tabs_);
    outputs_->setItemDelegate(new ConnectionDelegate([this](int row, int column) {
        QStringList result;
        if (!session_ || !document_) return result;
        const auto model = session_->connectionModel();
        if (row < 0 || static_cast<std::size_t>(row) >= model.rows().size()) return result;
        const auto validator = session_->connectionValidator();
        const auto& connection = model.rows()[static_cast<std::size_t>(row)].connection;
        std::vector<std::string> candidates;
        if (column == OutputName) {
            std::string common;
            bool same = true;
            for (std::size_t i = 0; i < model.entities().size(); ++i) {
                const auto* entity = document_->get(*document_->handle(model.entities()[i]));
                const auto* cls = entity->block->find("classname");
                const auto name = cls ? cls->decodedValue().value_or("") : "";
                if (i == 0) common = name;
                else same &= ciEquals(common, name);
            }
            if (same) candidates = editor::outputsFor(session_->entityClasses(), common);
        } else if (column == TargetColumn) {
            candidates = editor::targetsFor(session_->names(), "");
        } else if (column == InputColumn) {
            candidates = editor::inputsFor(validator, connection.target);
        } else if (column == ParameterColumn) {
            for (const auto target : validator.matching(connection.target)) {
                if (const auto* cls = validator.entityClass(target))
                    for (const auto& input : cls->inputs)
                        if (ciEquals(input.name, connection.input)) {
                            auto choices = editor::parameterChoices(session_->entityClasses(), input.type, session_->names());
                            candidates.insert(candidates.end(), std::make_move_iterator(choices.begin()),
                                              std::make_move_iterator(choices.end()));
                        }
            }
            std::ranges::sort(candidates, [](const auto& a, const auto& b) {
                return toLowerAscii(a) < toLowerAscii(b);
            });
            candidates.erase(std::unique(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                return ciEquals(a, b);
            }), candidates.end());
        }
        for (const auto& item : candidates) result.push_back(text(item));
        return result;
    }, outputs_));
    connect(outputs_->itemDelegate(), &QAbstractItemDelegate::closeEditor, this,
            [this](QWidget*, QAbstractItemDelegate::EndEditHint) {
                // Tab/Backtab opens the next cell editor before this runs; keep it. The stale-row
                // guard in itemChanged rejects an edit that follows a row-set change.
                const auto* focus = QApplication::focusWidget();
                if (focus && focus != outputs_ && outputs_->isAncestorOf(focus)) { requestRefresh(); return; }
                outputs_->setFocus(Qt::OtherFocusReason);
                refresh();
            });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 2) refreshConnectionTabs(true);
    });
    static_cast<ConnectionTable*>(outputs_)->keyHandler = [this](QKeyEvent* event) { handleConnectionKey(event); };
    outputs_->installEventFilter(this);
    inputs_->installEventFilter(this);
    connect(outputs_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (rebuilding_ || !session_ || !item) return;
        const int row = item->row(), column = item->column();
        if (!connectionRowMatches(row, column)) {
            QTimer::singleShot(0, this, [this] {
                outputs_->setFocus(Qt::OtherFocusReason);
                refresh();
            });
            return;
        }
        if (column == OutputName) session_->setConnectionOutput(static_cast<std::size_t>(row), item->text().toStdString());
        else if (column >= TargetColumn && column <= DelayColumn)
            session_->setConnectionField(static_cast<std::size_t>(row), static_cast<editor::ConnectionModel::Field>(column - 1), item->text().toStdString());
        else if (column == TimesColumn)
            session_->setConnectionField(static_cast<std::size_t>(row), document::ConnectionField::Times, item->text().toStdString());
        requestRefresh();
    });
    connect(inputs_, &QTableWidget::cellActivated, this, [this](int row, int) {
        if (!session_ || row < 0) return;
        const auto targets = session_->connectionModel().entities();
        const auto incoming = editor::incomingConnections(session_->connectionValidator(), *document_, targets);
        if (static_cast<std::size_t>(row) < incoming.size()) session_->selectNode(incoming[static_cast<std::size_t>(row)].source);
    });
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
    // Window-level shortcuts (File>New, Edit>Duplicate/Copy/Paste/Delete) would otherwise take
    // these keys from the table; accepting the override delivers them as ordinary key presses.
    if (object == outputs_ && event->type() == QEvent::ShortcutOverride && QApplication::focusWidget() == outputs_) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const bool control = key->modifiers() == Qt::ControlModifier;
        if ((control && (key->key() == Qt::Key_N || key->key() == Qt::Key_D || key->key() == Qt::Key_C ||
                         key->key() == Qt::Key_V)) ||
            (key->modifiers() == Qt::NoModifier && (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Insert))) {
            event->accept();
            return true;
        }
    }
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
    if (attached && focus && ((outputs_->isAncestorOf(focus) && focus != outputs_) ||
                              (inputs_->isAncestorOf(focus) && focus != inputs_))) return;
    const bool restoreOutputFocus = focus == outputs_;
    const int outputRow = outputs_->currentRow(), outputColumn = outputs_->currentColumn();
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
    if (rows_) { propertiesLayout_->removeWidget(rows_); delete rows_; }
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    rows_ = scroll;
    rows_->setProperty("raw", raw_->isChecked());
    propertiesLayout_->addWidget(scroll);
    auto* content = new QWidget(scroll);
    auto* form = new QFormLayout(content);
    scroll->setWidget(content);
    if (entities_.empty()) {
        classPicker_->setCurrentIndex(-1);
        form->addRow(new QLabel(tr("Select an entity to inspect."), content));
        refreshConnectionTabs(tabs_->currentWidget() == inputs_);
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
            auto* combo = new NoWheelComboBox(field);
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
                connect(edit, &QLineEdit::textEdited, this, [this, write, valid, index, component, edit, count, singleTargetname = entities.size() == 1 && ciEquals(row.key, "targetname")](const QString& value) {
                    if (!edit->hasAcceptableInput() || (count == 3 && value.isEmpty())) return;
                    // A single entity commits its targetname on editingFinished, after the reference prompt.
                    if (singleTargetname) return;
                    const auto focusKey = edit->property("inspectorFocus").toString();
                    if (count == 1) write(value, focusKey);
                    else if (valid()) session_->editVectorComponent(index, static_cast<std::size_t>(component),
                                                                     value.toStdString(), focusKey.toStdString());
                });
                if (count == 1 && entities.size() == 1 && ciEquals(row.key, "targetname")) {
                    connect(edit, &QLineEdit::editingFinished, this, [this, valid, edit = QPointer<QLineEdit>(edit), id = entities.front(), oldName = row.value] {
                        if (!edit || !valid() || !session_ || !document_ || session_->document() != document_) return;
                        const auto next = edit->text().toStdString();
                        if (next == oldName) return;
                        const auto stillValid = [this, valid, edit] {
                            return edit && session_ && document_ && session_->document() == document_ && valid();
                        };
                        if (!renameWithConfirmation(*session_, id, next, this, renamePresenter_, stillValid)) {
                            if (!edit) return;
                            edit->setText(text(oldName));
                            edit->setModified(false);
                        }
                    });
                }
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
        const QString label = row.property && !row.property->displayName.empty() ? text(row.property->displayName) : text(row.key);
        if (isSpacerLabel(label) || (row.readOnly && isSpacerLabel(text(row.value)))) {
            // Collapse the dash padding to a rule across the form.
            delete field;
            auto* rule = new QFrame(content);
            rule->setObjectName(QStringLiteral("property.%1.%2").arg(text(row.key)).arg(row.occurrence));
            rule->setFrameShape(QFrame::HLine);
            rule->setFrameShadow(QFrame::Sunken);
            rule->setFixedHeight(6);
            rule->setToolTip(row.property ? text(row.property->description) : QString{});
            form->addRow(rule);
            continue;
        }
        form->addRow(new ElidedLabel(label, content), field);
    }
    refreshConnectionTabs(tabs_->currentWidget() == inputs_);
    rebuilding_ = false;
    if (restoreOutputFocus) {
        outputs_->setFocus();
        if (outputRow >= 0 && outputRow < outputs_->rowCount())
            outputs_->setCurrentCell(outputRow, std::clamp(outputColumn, 0, outputs_->columnCount() - 1));
    }
}

bool InspectorPanel::connectionRowMatches(int row, int editedColumn) const {
    if (!session_ || row < 0 || static_cast<std::size_t>(row) >= displayedConnections_.size()) return false;
    const auto model = session_->connectionModel();
    if (static_cast<std::size_t>(row) >= model.rows().size()) return false;
    const auto& current = model.rows()[static_cast<std::size_t>(row)];
    const std::array<QString, 6> values{text(current.output), text(current.connection.target), text(current.connection.input),
        text(current.connection.parameter), text(current.connection.delay), text(current.connection.times)};
    const auto& previous = displayedConnections_[static_cast<std::size_t>(row)];
    for (int column = 0; column < 6; ++column)
        if (column != editedColumn && values[static_cast<std::size_t>(column)] != previous[static_cast<std::size_t>(column)]) return false;
    return true;
}

void InspectorPanel::refreshConnectionTabs(bool refreshInputs) {
    const bool restoreFocus = QApplication::focusWidget() == outputs_;
    const int oldRow = outputs_->currentRow(), oldColumn = outputs_->currentColumn();
    const QSignalBlocker outputBlock(outputs_);
    outputs_->clearContents();
    outputs_->setRowCount(0);
    if (refreshInputs) {
        inputs_->clearContents();
        inputs_->setRowCount(0);
    }
    displayedConnections_.clear();
    if (!session_ || !document_ || session_->document() != document_ || entities_.empty()) return;

    const auto model = session_->connectionModel();
    auto validator = session_->connectionValidator();
    const auto setCell = [this](int row, int column, QString value, bool editable = true) {
        auto* item = new QTableWidgetItem(std::move(value));
        item->setFlags(editable ? item->flags() | Qt::ItemIsEditable : item->flags() & ~Qt::ItemIsEditable);
        outputs_->setItem(row, column, item);
        return item;
    };
    outputs_->setRowCount(static_cast<int>(model.rows().size()));
    for (std::size_t row = 0; row < model.rows().size(); ++row) {
        const auto& entry = model.rows()[row];
        displayedConnections_.push_back({text(entry.output), text(entry.connection.target), text(entry.connection.input),
            text(entry.connection.parameter), text(entry.connection.delay), text(entry.connection.times)});
        setCell(static_cast<int>(row), OutputName, text(entry.output));
        setCell(static_cast<int>(row), TargetColumn, text(entry.connection.target));
        setCell(static_cast<int>(row), InputColumn, text(entry.connection.input));
        setCell(static_cast<int>(row), ParameterColumn, text(entry.connection.parameter));
        setCell(static_cast<int>(row), DelayColumn, text(entry.connection.delay));
        if (entry.connection.times == "1") {
            auto* once = new QCheckBox(outputs_);
            once->setObjectName(QStringLiteral("connectionOnce.%1").arg(row));
            once->setChecked(entry.connection.times == "1");
            once->setToolTip(tr("Checked means times = 1 (fire once). Unchecking sets the count to -1 (unlimited)."));
            outputs_->setCellWidget(static_cast<int>(row), TimesColumn, once);
            connect(once, &QCheckBox::toggled, this, [this, row](bool checked) {
                if (!rebuilding_ && session_) {
                    if (!connectionRowMatches(static_cast<int>(row))) {
                        QTimer::singleShot(0, this, [this] { refresh(); });
                        return;
                    }
                    session_->setConnectionField(row, document::ConnectionField::Times, checked ? "1" : "-1");
                    QTimer::singleShot(0, this, [this] {
                        outputs_->setFocus(Qt::OtherFocusReason);
                        refresh();
                    });
                }
            });
        } else {
            auto* item = setCell(static_cast<int>(row), TimesColumn, text(entry.connection.times));
            item->setToolTip(tr("Once is checked only when times = 1. Other values, including -1, are editable numbers."));
        }

        for (std::size_t entityIndex = 0; entityIndex < model.entities().size(); ++entityIndex) {
            auto issues = validator.validate(model.entities()[entityIndex], entry.connection, entry.output);
            for (const auto& issue : issues) {
                int column = TargetColumn;
                switch (issue.field) {
                    case editor::ConnectionIssue::Field::Output: column = OutputName; break;
                    case editor::ConnectionIssue::Field::Target: column = TargetColumn; break;
                    case editor::ConnectionIssue::Field::Input: column = InputColumn; break;
                    case editor::ConnectionIssue::Field::Parameter: column = ParameterColumn; break;
                    case editor::ConnectionIssue::Field::Delay: column = DelayColumn; break;
                    case editor::ConnectionIssue::Field::Times: column = TimesColumn; break;
                }
                if (auto* cell = outputs_->item(static_cast<int>(row), column)) {
                    cell->setBackground(QColor(255, 220, 220));
                    const auto old = cell->toolTip();
                    const auto message = issue.kind == editor::ConnectionIssueKind::Malformed
                        ? tr("editing rewrites this row as five fields") : text(issue.message);
                    cell->setToolTip(old.isEmpty() ? message : old + '\n' + message);
                } else if (column == TimesColumn) {
                    if (auto* check = qobject_cast<QCheckBox*>(outputs_->cellWidget(static_cast<int>(row), column)))
                        check->setToolTip(check->toolTip() + '\n' + (issue.kind == editor::ConnectionIssueKind::Malformed
                            ? tr("editing rewrites this row as five fields") : text(issue.message)));
                }
            }
        }
    }
    outputs_->resizeColumnsToContents();

    if (refreshInputs) {
    const auto incoming = editor::incomingConnections(validator, *document_, model.entities());
    inputs_->setRowCount(static_cast<int>(incoming.size()));
    for (std::size_t row = 0; row < incoming.size(); ++row) {
        const auto& item = incoming[row];
        const auto handle = document_->handle(item.source);
        const auto* source = handle ? document_->get(*handle) : nullptr;
        const auto* cls = source && source->block ? source->block->find("classname") : nullptr;
        const auto* name = source && source->block ? source->block->find("targetname") : nullptr;
        QString label = cls ? text(cls->decodedValue().value_or("")) : tr("entity");
        if (name && !name->decodedValue().value_or("").empty()) label += QStringLiteral(" (%1)").arg(text(name->decodedValue().value_or("")));
        const auto fields = item.connection.fields();
        const QStringList values{label, text(item.output), text(fields[1]), text(fields[2]), text(fields[3])};
        for (int column = 0; column < values.size(); ++column)
            inputs_->setItem(static_cast<int>(row), column, new QTableWidgetItem(values[column]));
        inputs_->item(static_cast<int>(row), 0)->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(item.source));
    }
    }
    if (restoreFocus) {
        outputs_->setFocus();
        if (oldRow >= 0 && oldRow < outputs_->rowCount()) outputs_->setCurrentCell(oldRow, std::clamp(oldColumn, 0, outputs_->columnCount() - 1));
    }
    if (pendingOutputEdit_) {
        const auto row = *pendingOutputEdit_;
        pendingOutputEdit_.reset();
        if (row < static_cast<std::size_t>(outputs_->rowCount())) {
            outputs_->setCurrentCell(static_cast<int>(row), OutputName);
            outputs_->setFocus();
            QTimer::singleShot(0, this, [this, row] {
                if (row < static_cast<std::size_t>(outputs_->rowCount()))
                    outputs_->editItem(outputs_->item(static_cast<int>(row), OutputName));
            });
        }
    }
}

void InspectorPanel::handleConnectionKey(QKeyEvent* event) {
    if (!session_ || rebuilding_) return;
    const auto modifiers = event->modifiers();
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    const auto key = event->key();
    void (InspectorPanel::*action)() = nullptr;
    if ((control && key == Qt::Key_N) || key == Qt::Key_Insert) action = &InspectorPanel::addConnectionRow;
    else if (key == Qt::Key_Delete) action = &InspectorPanel::deleteConnectionRows;
    else if (control && key == Qt::Key_D) action = &InspectorPanel::duplicateConnectionRow;
    else if (control && key == Qt::Key_C) action = &InspectorPanel::copyConnectionRows;
    else if (control && key == Qt::Key_V) action = &InspectorPanel::pasteConnectionRows;
    if (!action) return;
    (this->*action)();
    event->accept();
}

void InspectorPanel::addConnectionRow() {
    if (!session_ || rebuilding_ || entities_.empty()) return;
    const auto current = outputs_->currentRow();
    if (current >= 0 && !connectionRowMatches(current)) { refresh(); return; }
    auto connection = document::parseConnection(std::string("\x1b\x1b\x1b") + "0\x1b-1");
    const auto model = session_->connectionModel();
    const auto after = current >= 0 ? std::optional(static_cast<std::size_t>(current)) : std::nullopt;
    pendingOutputEdit_ = after ? *after + 1 : model.rows().size();
    tabs_->setCurrentWidget(outputs_->parentWidget());
    if (session_->addConnection({}, std::move(connection), after)) refresh();
}

void InspectorPanel::deleteConnectionRows() {
    if (!session_ || rebuilding_) return;
    const auto current = outputs_->currentRow();
    auto rows = outputs_->selectionModel()->selectedRows();
    std::vector<int> indices;
    for (const auto& index : rows) indices.push_back(index.row());
    if (indices.empty() && current >= 0) indices.push_back(current);
    std::ranges::sort(indices);
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (std::ranges::all_of(indices, [&](int row) { return connectionRowMatches(row); })) {
        std::vector<std::size_t> selectedRows;
        for (const auto row : indices) if (row >= 0) selectedRows.push_back(static_cast<std::size_t>(row));
        if (!selectedRows.empty()) session_->removeConnections(std::move(selectedRows));
    } else refresh();
    requestRefresh();
}

void InspectorPanel::duplicateConnectionRow() {
    if (!session_ || rebuilding_) return;
    const auto current = outputs_->currentRow();
    if (current >= 0 && connectionRowMatches(current)) session_->duplicateConnection(static_cast<std::size_t>(current));
    else if (current >= 0) refresh();
    requestRefresh();
}

void InspectorPanel::copyConnectionRows() {
    if (!session_ || rebuilding_) return;
    const auto current = outputs_->currentRow();
    auto selected = outputs_->selectionModel()->selectedRows();
    if (selected.empty() && current >= 0) selected.push_back(outputs_->model()->index(current, 0));
    std::vector<int> rows;
    for (const auto& index : selected) rows.push_back(index.row());
    std::ranges::sort(rows);
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    const auto model = session_->connectionModel();
    QStringList lines;
    for (const auto row : rows) {
        if (row < 0 || static_cast<std::size_t>(row) >= model.rows().size()) continue;
        QStringList values{text(model.rows()[static_cast<std::size_t>(row)].output)};
        for (const auto& field : model.rows()[static_cast<std::size_t>(row)].connection.fields()) values.push_back(text(field));
        lines.push_back(values.join('\t'));
    }
    QGuiApplication::clipboard()->setText(lines.join('\n'));
}

void InspectorPanel::pasteConnectionRows() {
    if (!session_ || rebuilding_ || entities_.empty()) return;
    const auto current = outputs_->currentRow();
    const auto clipboard = QGuiApplication::clipboard()->text();
    std::optional<std::size_t> after = current >= 0 ? std::optional(static_cast<std::size_t>(current)) : std::nullopt;
    std::vector<std::pair<std::string, document::Connection>> additions;
    for (auto line : clipboard.split('\n', Qt::SkipEmptyParts)) {
        if (line.endsWith('\r')) line.chop(1);
        const auto values = line.split('\t');
        if (values.size() != 6) { additions.clear(); break; }
        const auto esc = QChar(0x1b);
        QStringList fields;
        for (qsizetype i = 1; i < values.size(); ++i) fields.push_back(values[i]);
        const QString encoded = fields.join(esc);
        auto connection = document::parseConnection(encoded.toStdString());
        if (!connection.wellFormed) { additions.clear(); break; }
        additions.emplace_back(values[0].toStdString(), std::move(connection));
    }
    if (!additions.empty() && (current < 0 || connectionRowMatches(current)))
        session_->addConnections(std::move(additions), after);
    requestRefresh();
}

void InspectorPanel::showConnectionMenu(const QPoint& position) {
    if (!session_ || entities_.empty()) return;
    const auto index = outputs_->indexAt(position);
    if (index.isValid() && !outputs_->selectionModel()->isRowSelected(index.row(), QModelIndex()))
        outputs_->selectRow(index.row());
    if (index.isValid()) outputs_->setCurrentCell(index.row(), index.column());
    const bool hasRow = index.isValid() || outputs_->currentRow() >= 0;
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("inspectorOutputsMenu"));
    // The shortcut text after the tab is display-only: the keys are handled by the table itself.
    const auto add = [&](const QString& name, const QString& label, bool enabled, void (InspectorPanel::*action)()) {
        auto* item = menu.addAction(label);
        item->setObjectName(name);
        item->setEnabled(enabled);
        connect(item, &QAction::triggered, this, action);
    };
    add(QStringLiteral("menuAddOutput"), tr("Add Output\tCtrl+N"), true, &InspectorPanel::addConnectionRow);
    add(QStringLiteral("menuDuplicate"), tr("Duplicate\tCtrl+D"), hasRow, &InspectorPanel::duplicateConnectionRow);
    menu.addSeparator();
    add(QStringLiteral("menuCopy"), tr("Copy\tCtrl+C"), hasRow, &InspectorPanel::copyConnectionRows);
    add(QStringLiteral("menuPaste"), tr("Paste\tCtrl+V"), QGuiApplication::clipboard()->text().contains('\t'),
        &InspectorPanel::pasteConnectionRows);
    menu.addSeparator();
    add(QStringLiteral("menuDelete"), tr("Delete\tDel"), hasRow, &InspectorPanel::deleteConnectionRows);
    if (menuHook_) menuHook_(&menu); else menu.exec(outputs_->viewport()->mapToGlobal(position));
}

} // namespace apex::ui
