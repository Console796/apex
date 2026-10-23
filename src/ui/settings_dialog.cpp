#include "ui/settings_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "editor/preset_io.hpp"
#include "ui/key_chord.hpp"
#include "ui/main_window.hpp"
#include "ui/theme.hpp"
#include "ui/theme_editor.hpp"

namespace apex::ui {
namespace {
QString fromStd(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

constexpr int kIdRole = Qt::UserRole;

QString chordsText(const editor::Binding& binding) {
    const auto* keys = std::get_if<std::vector<editor::KeyChord>>(&binding.value);
    if (!keys) return {};
    QStringList parts;
    for (const auto& chord : *keys) parts << fromStd(chord.toString());
    return parts.join(QStringLiteral(", "));
}

bool sameBinding(const editor::Binding& a, const editor::Binding& b) {
    if (a.context != b.context || a.value.index() != b.value.index()) return false;
    const auto* keys = std::get_if<std::vector<editor::KeyChord>>(&a.value);
    return !keys || *keys == std::get<std::vector<editor::KeyChord>>(b.value);
}

bool isModifierKey(int key) {
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta ||
           key == Qt::Key_AltGr;
}
}  // namespace

// ---- KeyCaptureDialog ----------------------------------------------------------------------

KeyCaptureDialog::KeyCaptureDialog(const QString& actionName, bool allowHeldModifier, QWidget* parent)
    : QDialog(parent), allowHeld_(allowHeldModifier) {
    setObjectName(QStringLiteral("keyCaptureDialog"));
    setWindowTitle(tr("Set key"));
    setModal(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Press the key combination for <b>%1</b>.").arg(actionName.toHtmlEscaped())));
    hint_ = new QLabel(allowHeld_ ? tr("Press Ctrl, Alt or Shift alone to bind the held key.") : QString());
    hint_->setObjectName(QStringLiteral("keyCaptureHint"));
    layout->addWidget(hint_);
    auto* cancel = new QPushButton(tr("Cancel"));
    cancel->setFocusPolicy(Qt::NoFocus);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancel, 0, Qt::AlignRight);
    setFocusPolicy(Qt::StrongFocus);
}

bool KeyCaptureDialog::event(QEvent* e) {
    // Take every key, including ones a window shortcut would claim.
    if (e->type() == QEvent::ShortcutOverride) {
        e->accept();
        return true;
    }
    return QDialog::event(e);
}

void KeyCaptureDialog::keyPressEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    if (isModifierKey(e->key())) {
        if (!sawKey_) {
            switch (e->key()) {
            case Qt::Key_Control: pendingModifier_ = QStringLiteral("Ctrl"); break;
            case Qt::Key_Alt: pendingModifier_ = QStringLiteral("Alt"); break;
            case Qt::Key_Shift: pendingModifier_ = QStringLiteral("Shift"); break;
            default: pendingModifier_.clear(); break;
            }
        }
        return;
    }
    sawKey_ = true;
    pendingModifier_.clear();
    if (auto chord = keyChord(*e)) {
        chord_ = std::move(chord);
        accept();
    } else {
        hint_->setText(tr("That key cannot be bound. Try another."));
    }
}

void KeyCaptureDialog::keyReleaseEvent(QKeyEvent* e) {
    if (e->isAutoRepeat() || !allowHeld_ || sawKey_ || pendingModifier_.isEmpty() || !isModifierKey(e->key())) return;
    if (auto chord = editor::KeyChord::parse(pendingModifier_.toStdString())) {
        chord_ = std::move(*chord);
        accept();
    }
}

// ---- SettingsDialog ------------------------------------------------------------------------

SettingsDialog::SettingsDialog(MainWindow& window) : QDialog(&window), window_(window) {
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(tr("Preferences"));
    resize(900, 600);

    auto* pages = new QStackedWidget;
    pages->addWidget(buildGeneralPage());
    pages->addWidget(buildKeysPage());
    auto* nav = new QListWidget;
    nav->setObjectName(QStringLiteral("settingsNav"));
    nav->setFixedWidth(150);
    nav->addItems({tr("General"), tr("Keybindings")});
    connect(nav, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    nav->setCurrentRow(0);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { if (confirmDiscard()) reject(); });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { applyAll(); });
    connect(buttons->button(QDialogButtonBox::Ok), &QPushButton::clicked, this, [this] { if (applyAll()) accept(); });

    auto* body = new QHBoxLayout;
    body->addWidget(nav);
    body->addWidget(pages, 1);
    auto* root = new QVBoxLayout(this);
    root->addLayout(body, 1);
    root->addWidget(buttons);

    loadPreset(window_.editorSession().preset().name);
}

void SettingsDialog::refreshThemeCombo(const QString& select) {
    const QSignalBlocker block(theme_);
    theme_->clear();
    for (const ThemeEntry& t : themes()) theme_->addItem(t.builtin ? t.name : tr("%1 (custom)").arg(t.name), t.id);
    const int i = theme_->findData(select);
    theme_->setCurrentIndex(i < 0 ? 0 : i);
}

void SettingsDialog::updateThemeButtons() {
    const ThemeEntry* t = findTheme(theme_->currentData().toString());
    const bool custom = t && !t->builtin;
    themeEdit_->setEnabled(custom);
    themeDelete_->setEnabled(custom);
}

void SettingsDialog::editTheme(bool fromSelected) {
    const ThemeEntry* sel = findTheme(theme_->currentData().toString());
    if (!sel) return;
    ThemeEntry initial = *sel;
    if (fromSelected || sel->builtin) {
        initial.id.clear();
        initial.builtin = false;
        initial.name = tr("%1 copy").arg(sel->name);
    }
    ThemeEditorDialog dlg(window_, initial, this);
    const bool accepted = dlg.exec() == QDialog::Accepted;
    window_.refreshThemeMenu();
    refreshThemeCombo(accepted ? dlg.savedId() : currentThemeId());
    updateThemeButtons();
}

void SettingsDialog::deleteTheme() {
    const QString id = theme_->currentData().toString();
    const ThemeEntry* t = findTheme(id);
    if (!t || t->builtin) return;
    if (QMessageBox::question(this, tr("Delete theme"), tr("Delete the theme \"%1\"?").arg(t->name)) != QMessageBox::Yes) return;
    const bool wasCurrent = id == currentThemeId();
    removeCustomTheme(id);
    if (wasCurrent) window_.setThemeId(QStringLiteral("dark"));
    window_.refreshThemeMenu();
    refreshThemeCombo(currentThemeId());
    updateThemeButtons();
}

void SettingsDialog::importTheme() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import theme"), {}, tr("Theme files (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    const QString id = importThemeFile(path, themesDir(), &error);
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("Import theme"), tr("Cannot import %1: %2").arg(path, error));
        return;
    }
    window_.refreshThemeMenu();
    refreshThemeCombo(id);
    updateThemeButtons();
}

void SettingsDialog::exportTheme() {
    const QString id = theme_->currentData().toString();
    const ThemeEntry* t = findTheme(id);
    if (!t) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export theme"), t->name + QStringLiteral(".json"), tr("Theme files (*.json)"));
    if (path.isEmpty()) return;
    if (!apex::ui::exportTheme(id, path)) QMessageBox::warning(this, tr("Export theme"), tr("Cannot write %1").arg(path));
}

QWidget* SettingsDialog::buildGeneralPage() {
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    theme_ = new QComboBox;
    theme_->setObjectName(QStringLiteral("settingsTheme"));
    refreshThemeCombo(currentThemeId());
    form->addRow(tr("Theme"), theme_);

    auto* themeButtons = new QHBoxLayout;
    const auto themeButton = [&](const QString& text, const char* name) {
        auto* b = new QPushButton(text);
        b->setObjectName(QString::fromLatin1(name));
        themeButtons->addWidget(b);
        return b;
    };
    auto* newTheme = themeButton(tr("New from selected…"), "themeNew");
    themeEdit_ = themeButton(tr("Edit…"), "themeEdit");
    themeDelete_ = themeButton(tr("Delete"), "themeDelete");
    auto* importTheme = themeButton(tr("Import…"), "themeImport");
    themeExport_ = themeButton(tr("Export…"), "themeExport");
    themeButtons->addStretch(1);
    form->addRow(QString(), themeButtons);
    connect(theme_, &QComboBox::currentIndexChanged, this, [this] { updateThemeButtons(); });
    connect(newTheme, &QPushButton::clicked, this, [this] { editTheme(true); });
    connect(themeEdit_, &QPushButton::clicked, this, [this] { editTheme(false); });
    connect(themeDelete_, &QPushButton::clicked, this, &SettingsDialog::deleteTheme);
    connect(importTheme, &QPushButton::clicked, this, &SettingsDialog::importTheme);
    connect(themeExport_, &QPushButton::clicked, this, &SettingsDialog::exportTheme);
    updateThemeButtons();

    autosave_ = new QCheckBox(tr("Enable autosave"));
    autosave_->setObjectName(QStringLiteral("settingsAutosave"));
    autosave_->setChecked(QSettings().value("autosave/enabled", true).toBool());
    autosaveMinutes_ = new QSpinBox;
    autosaveMinutes_->setObjectName(QStringLiteral("settingsAutosaveMinutes"));
    autosaveMinutes_->setRange(1, 1440);
    autosaveMinutes_->setSuffix(tr(" min"));
    autosaveMinutes_->setValue(QSettings().value("autosave/intervalMinutes", 5).toInt());
    autosaveMinutes_->setEnabled(autosave_->isChecked());
    connect(autosave_, &QCheckBox::toggled, autosaveMinutes_, &QWidget::setEnabled);
    form->addRow(tr("Autosave"), autosave_);
    form->addRow(tr("Autosave interval"), autosaveMinutes_);

    undoMiB_ = new QSpinBox;
    undoMiB_->setObjectName(QStringLiteral("settingsUndoMiB"));
    undoMiB_->setRange(16, 8192);
    undoMiB_->setSingleStep(16);
    undoMiB_->setSuffix(tr(" MiB"));
    bool valid = false;
    const auto cap = QSettings().value(QStringLiteral("history/undoCapMiB"), 512).toInt(&valid);
    undoMiB_->setValue(valid ? std::clamp(cap, 16, 8192) : 512);
    form->addRow(tr("Undo memory limit"), undoMiB_);

    inspectorOpens_ = new QComboBox;
    inspectorOpens_->setObjectName(QStringLiteral("settingsInspectorOpens"));
    inspectorOpens_->addItem(tr("Workspace area"), false);
    inspectorOpens_->addItem(tr("Floating window"), true);
    inspectorOpens_->setCurrentIndex(MainWindow::inspectorInWindow() ? 1 : 0);
    inspectorOpens_->setToolTip(tr("Where Object properties shows the inspector. Without an inspector area in the "
                                   "workspace it always opens a window."));
    form->addRow(tr("Object properties opens in"), inspectorOpens_);
    return page;
}

QWidget* SettingsDialog::buildKeysPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Preset:")));
    presetCombo_ = new QComboBox;
    presetCombo_->setObjectName(QStringLiteral("keyPresetCombo"));
    presetCombo_->setMinimumWidth(180);
    top->addWidget(presetCombo_);
    modified_ = new QLabel(tr("(modified)"));
    modified_->setObjectName(QStringLiteral("keyPresetModified"));
    top->addWidget(modified_);
    top->addStretch(1);
    saveButton_ = new QPushButton(tr("Save"));
    saveButton_->setObjectName(QStringLiteral("keySave"));
    saveAsButton_ = new QPushButton(tr("Save As..."));
    saveAsButton_->setObjectName(QStringLiteral("keySaveAs"));
    deleteButton_ = new QPushButton(tr("Delete"));
    deleteButton_->setObjectName(QStringLiteral("keyDelete"));
    auto* import = new QPushButton(tr("Import..."));
    auto* exportButton = new QPushButton(tr("Export..."));
    for (auto* b : {saveButton_, saveAsButton_, deleteButton_, import, exportButton}) top->addWidget(b);
    layout->addLayout(top);

    filter_ = new QLineEdit;
    filter_->setObjectName(QStringLiteral("keyFilter"));
    filter_->setPlaceholderText(tr("Filter by action or key"));
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);

    tree_ = new QTreeWidget;
    tree_->setObjectName(QStringLiteral("keyTree"));
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Action"), tr("Keys"), tr("When")});
    tree_->setRootIsDecorated(true);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(tree_, 1);

    detail_ = new QLabel;
    detail_->setObjectName(QStringLiteral("keyDetail"));
    detail_->setWordWrap(true);
    detail_->setMinimumHeight(detail_->fontMetrics().height() * 3);
    detail_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(detail_);

    auto* row = new QHBoxLayout;
    assignButton_ = new QPushButton(tr("Set Key..."));
    assignButton_->setObjectName(QStringLiteral("keyAssign"));
    addButton_ = new QPushButton(tr("Add Alternate..."));
    addButton_->setObjectName(QStringLiteral("keyAdd"));
    clearButton_ = new QPushButton(tr("Clear"));
    clearButton_->setObjectName(QStringLiteral("keyClear"));
    resetButton_ = new QPushButton(tr("Reset"));
    resetButton_->setObjectName(QStringLiteral("keyReset"));
    resetButton_->setToolTip(tr("Restore this action's binding from the loaded preset."));
    for (auto* b : {assignButton_, addButton_, clearButton_, resetButton_}) row->addWidget(b);
    row->addStretch(1);
    layout->addLayout(row);

    connect(presetCombo_, &QComboBox::activated, this, [this](int i) {
        const auto name = presetCombo_->itemData(i).toString().toStdString();
        if (name == original_.name || !confirmDiscard()) { refreshPresetCombo(); return; }
        loadPreset(name);
    });
    connect(filter_, &QLineEdit::textChanged, this, [this] { refreshTree(); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { refreshButtons(); });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item->parent()) captureKey(false);
    });
    connect(assignButton_, &QPushButton::clicked, this, [this] { captureKey(false); });
    connect(addButton_, &QPushButton::clicked, this, [this] { captureKey(true); });
    connect(clearButton_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedAction(); !id.empty()) assign(id, {});
    });
    connect(resetButton_, &QPushButton::clicked, this, [this] { resetSelected(); });
    connect(saveButton_, &QPushButton::clicked, this, [this] { saveCurrent(); });
    connect(saveAsButton_, &QPushButton::clicked, this, [this] { saveAs(); });
    connect(deleteButton_, &QPushButton::clicked, this, [this] { deletePreset(); });
    connect(import, &QPushButton::clicked, this, [this] { importPreset(); });
    connect(exportButton, &QPushButton::clicked, this, [this] { exportPreset(); });
    return page;
}

bool SettingsDialog::keysModified() const {
    return editor::serializePreset(working_) != editor::serializePreset(original_);
}

void SettingsDialog::loadPreset(const std::string& name) {
    auto preset = window_.presetStore().find(name);
    if (!preset) {
        log::warn("keybinding preset '{}': {}", name, preset.error().what());
        preset = editor::hammerPreset();
    }
    original_ = *preset;
    working_ = *preset;
    baseIsBuiltin_ = editor::findPreset(preset->name) != nullptr;
    refreshPresetCombo();
    refreshTree();
}

void SettingsDialog::refreshPresetCombo() {
    const QSignalBlocker block(presetCombo_);
    presetCombo_->clear();
    for (const auto name : editor::presetNames())
        presetCombo_->addItem(fromStd(name) + tr(" (built-in)"), fromStd(name));
    for (const auto& name : window_.presetStore().list()) presetCombo_->addItem(fromStd(name), fromStd(name));
    const int i = presetCombo_->findData(fromStd(original_.name));
    if (i >= 0) presetCombo_->setCurrentIndex(i);
    refreshButtons();
}

void SettingsDialog::refreshTree() {
    const QString query = filter_ ? filter_->text().trimmed() : QString();
    const auto selected = selectedAction();
    QSignalBlocker block(tree_);
    tree_->clear();
    std::map<std::string, QTreeWidgetItem*> categories;
    for (const auto& action : editor::actionRegistry().entries()) {
        const auto& binding = working_.bindings.at(action.id);
        const QString keys = chordsText(binding);
        if (!query.isEmpty() && !fromStd(action.name).contains(query, Qt::CaseInsensitive) &&
            !fromStd(action.category).contains(query, Qt::CaseInsensitive) &&
            !keys.contains(query, Qt::CaseInsensitive))
            continue;
        auto*& parent = categories[action.category];
        if (!parent) {
            parent = new QTreeWidgetItem(tree_, QStringList{fromStd(action.category)});
            QFont font = parent->font(0);
            font.setBold(true);
            parent->setFont(0, font);
            parent->setFlags(Qt::ItemIsEnabled);
        }
        auto* item = new QTreeWidgetItem(parent);
        item->setData(0, kIdRole, fromStd(action.id));
        item->setText(0, fromStd(action.name));
        item->setText(1, keys.isEmpty() ? QStringLiteral("—") : keys);
        if (binding.context == editor::BindingContext::SelectToolActive)
            item->setText(2, tr("Select tool active"));
        const auto* original = original_.bindings.contains(action.id) ? &original_.bindings.at(action.id) : nullptr;
        if (original && !sameBinding(*original, binding)) {
            QFont font = item->font(1);
            font.setItalic(true);
            font.setBold(true);
            item->setFont(1, font);
        }
        if (fromStd(action.id) == fromStd(selected)) tree_->setCurrentItem(item);
    }
    tree_->expandAll();
    block.unblock();
    refreshButtons();
}

editor::ActionId SettingsDialog::selectedAction() const {
    if (!tree_) return {};
    const auto* item = tree_->currentItem();
    return item && item->parent() ? item->data(0, kIdRole).toString().toStdString() : editor::ActionId{};
}

void SettingsDialog::refreshButtons() {
    if (!tree_ || !saveButton_) return;
    const bool dirty = keysModified();
    modified_->setVisible(dirty);
    saveButton_->setEnabled(dirty && !baseIsBuiltin_);
    deleteButton_->setEnabled(!baseIsBuiltin_);
    const auto id = selectedAction();
    const bool hasAction = !id.empty();
    for (auto* b : {assignButton_, addButton_, clearButton_, resetButton_}) b->setEnabled(hasAction);
    if (!hasAction) {
        detail_->setText(tr("Select an action to change its keys."));
        return;
    }
    const auto* action = editor::actionRegistry().find(id);
    const auto& binding = working_.bindings.at(id);
    clearButton_->setEnabled(!std::holds_alternative<editor::Unbound>(binding.value));
    resetButton_->setEnabled(!sameBinding(binding, original_.bindings.at(id)));
    QString text = QStringLiteral("<b>%1</b><br>%2").arg(fromStd(action->name).toHtmlEscaped(),
                                                       fromStd(action->description).toHtmlEscaped());
    if (const auto* unbound = std::get_if<editor::Unbound>(&binding.value))
        text += QStringLiteral("<br><i>%1</i>").arg(fromStd(unbound->reason).toHtmlEscaped());
    detail_->setText(text);
}

void SettingsDialog::captureKey(bool additional) {
    const auto id = selectedAction();
    if (id.empty()) return;
    const auto* action = editor::actionRegistry().find(id);
    KeyCaptureDialog capture(fromStd(action->name), editor::allowsHeldModifier(id), this);
    if (capture.exec() != QDialog::Accepted || !capture.chord()) return;
    const auto& chord = *capture.chord();
    // A held-key binding stands alone; a bare modifier is meaningless anywhere else.
    if ((chord.key == "Ctrl" || chord.key == "Alt" || chord.key == "Shift")) additional = false;
    std::vector<editor::KeyChord> chords;
    if (additional) {
        if (const auto* keys = std::get_if<std::vector<editor::KeyChord>>(&working_.bindings.at(id).value))
            chords = *keys;
    }
    if (std::ranges::find(chords, chord) == chords.end()) chords.push_back(chord);
    if (const auto owner = editor::chordOwner(working_, chord, id)) {
        const auto* other = editor::actionRegistry().find(*owner);
        const auto answer = QMessageBox::question(
            this, tr("Key already in use"),
            tr("%1 is already bound to \"%2\".\nReassign it to \"%3\"?")
                .arg(fromStd(chord.toString()), fromStd(other->name), fromStd(action->name)));
        if (answer != QMessageBox::Yes) return;
    }
    assign(id, std::move(chords));
}

void SettingsDialog::assign(const editor::ActionId& id, std::vector<editor::KeyChord> chords) {
    editor::assignChords(working_, id, std::move(chords));
    refreshTree();
}

void SettingsDialog::resetSelected() {
    const auto id = selectedAction();
    if (id.empty()) return;
    const auto& base = original_.bindings.at(id);
    if (const auto* keys = std::get_if<std::vector<editor::KeyChord>>(&base.value)) {
        editor::assignChords(working_, id, *keys);
        working_.bindings[id] = base;
    } else {
        working_.bindings[id] = base;
    }
    refreshTree();
}

bool SettingsDialog::saveAs() {
    QString suggestion = baseIsBuiltin_ ? fromStd(working_.name) + tr(" custom") : fromStd(working_.name);
    while (true) {
        bool accepted = false;
        const auto text = QInputDialog::getText(this, tr("Save preset"), tr("Preset name:"), QLineEdit::Normal,
                                                suggestion, &accepted);
        if (!accepted) return false;
        const auto name = editor::checkPresetName(text.toStdString());
        if (!name) {
            QMessageBox::warning(this, tr("Save preset"), fromStd(name.error().message));
            suggestion = text;
            continue;
        }
        const auto existing = window_.presetStore().list();
        if (std::ranges::find(existing, *name) != existing.end() && *name != original_.name &&
            QMessageBox::question(this, tr("Save preset"), tr("Replace the existing preset \"%1\"?").arg(fromStd(*name))) !=
                QMessageBox::Yes) {
            suggestion = text;
            continue;
        }
        editor::Preset preset = working_;
        preset.name = *name;
        if (auto saved = window_.presetStore().save(preset); !saved) {
            QMessageBox::warning(this, tr("Save preset"), fromStd(saved.error().what()));
            return false;
        }
        working_ = preset;
        original_ = preset;
        baseIsBuiltin_ = false;
        window_.refreshPresetMenu();
        refreshPresetCombo();
        refreshTree();
        return true;
    }
}

bool SettingsDialog::saveCurrent() {
    if (baseIsBuiltin_) return saveAs();
    if (auto saved = window_.presetStore().save(working_); !saved) {
        QMessageBox::warning(this, tr("Save preset"), fromStd(saved.error().what()));
        return false;
    }
    original_ = working_;
    refreshTree();
    return true;
}

void SettingsDialog::deletePreset() {
    if (baseIsBuiltin_) return;
    if (QMessageBox::question(this, tr("Delete preset"), tr("Delete the preset \"%1\"?").arg(fromStd(original_.name))) !=
        QMessageBox::Yes)
        return;
    if (auto removed = window_.presetStore().remove(original_.name); !removed) {
        QMessageBox::warning(this, tr("Delete preset"), fromStd(removed.error().what()));
        return;
    }
    const bool active = window_.editorSession().preset().name == original_.name;
    window_.refreshPresetMenu();
    if (active) window_.applyPreset(editor::hammerPreset());
    loadPreset(window_.editorSession().preset().name);
}

void SettingsDialog::importPreset() {
    if (!confirmDiscard()) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("Import keybinding preset"), QString(),
                                                   tr("Keybinding presets (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import"), tr("Cannot read %1").arg(path));
        return;
    }
    auto preset = editor::parsePreset(file.readAll().toStdString());
    if (!preset) {
        QMessageBox::warning(this, tr("Import"), fromStd(preset.error().what()));
        return;
    }
    // An imported preset is saved under a name of the user's choosing, or not at all.
    if (!editor::checkPresetName(preset->name)) preset->name = "Imported";
    const auto previous = working_.name;
    working_ = *preset;
    original_ = *preset;
    baseIsBuiltin_ = true; // Forces Save As, which validates the name and writes the file.
    if (!saveAs()) loadPreset(previous);
}

void SettingsDialog::exportPreset() {
    const auto path = QFileDialog::getSaveFileName(this, tr("Export keybinding preset"),
                                                   fromStd(working_.name) + QStringLiteral(".json"),
                                                   tr("Keybinding presets (*.json)"));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(editor::serializePreset(working_).c_str()) < 0 || !file.commit())
        QMessageBox::warning(this, tr("Export"), tr("Cannot write %1").arg(path));
}

bool SettingsDialog::confirmDiscard() {
    if (!keysModified()) return true;
    const auto answer = QMessageBox::question(this, tr("Unsaved keybinding changes"),
                                              tr("Discard the unapplied changes to \"%1\"?").arg(fromStd(working_.name)));
    return answer == QMessageBox::Yes;
}

bool SettingsDialog::applyAll() {
    if (theme_->currentData().toString() != currentThemeId()) window_.setThemeId(theme_->currentData().toString());
    window_.setAutosave(autosave_->isChecked(), autosaveMinutes_->value());
    MainWindow::setInspectorInWindow(inspectorOpens_->currentData().toBool());
    if (undoMiB_->value() != QSettings().value(QStringLiteral("history/undoCapMiB"), 512).toInt())
        window_.setUndoCapMiB(undoMiB_->value());

    // A keymap is applied only once saved, so the remembered preset name always reloads it.
    if (keysModified() || working_.name != window_.editorSession().preset().name) {
        if (keysModified() && !saveCurrent()) return false;
        window_.applyPreset(working_);
        window_.refreshPresetMenu();
        refreshTree();
    }
    return true;
}

}  // namespace apex::ui
