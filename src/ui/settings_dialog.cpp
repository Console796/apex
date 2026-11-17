#include "ui/settings_dialog.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
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

#include <algorithm>
#include <tuple>

#include "core/log.hpp"
#include "editor/pie_menus.hpp"
#include "editor/preset_io.hpp"
#include "plugin_host/plugin_host.hpp"
#include "ui/bake_controller.hpp"
#include "ui/game_content.hpp"
#include "ui/key_chord.hpp"
#include "ui/main_window.hpp"
#include "ui/pie_menu_popup.hpp"
#include "ui/theme.hpp"
#include "ui/theme_editor.hpp"
#include "ui/vrad_controller.hpp"

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
    pages->addWidget(buildInputPage());
    pages->addWidget(buildPiesPage());
    auto* nav = new QListWidget;
    nav->setObjectName(QStringLiteral("settingsNav"));
    nav->setFixedWidth(150);
    nav->addItems({tr("General"), tr("Keybindings"), tr("Input"), tr("Pies")});
    connect(nav, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    nav->setCurrentRow(0);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { reject(); });
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

void SettingsDialog::reject() {
    if (confirmDiscard()) QDialog::reject();
}

void SettingsDialog::refreshThemeCombo(const QString& select) {
    const QSignalBlocker block(theme_);
    theme_->clear();
    for (const ThemeEntry& t : themes()) {
        const QString label = t.builtin ? t.name : isPluginTheme(t) ? tr("%1 (plugin %2)").arg(t.name, t.plugin) : tr("%1 (custom)").arg(t.name);
        theme_->addItem(label, t.id);
    }
    const int i = theme_->findData(select);
    theme_->setCurrentIndex(i < 0 ? 0 : i);
}

void SettingsDialog::updateThemeButtons() {
    const ThemeEntry* t = findTheme(theme_->currentData().toString());
    const bool custom = t && !t->builtin && !isPluginTheme(*t);  // Plugin themes are read-only.
    themeEdit_->setEnabled(custom);
    themeDelete_->setEnabled(custom);
}

void SettingsDialog::editTheme(bool fromSelected) {
    const ThemeEntry* sel = findTheme(theme_->currentData().toString());
    if (!sel) return;
    ThemeEntry initial = *sel;
    if (fromSelected || sel->builtin || isPluginTheme(*sel)) {
        initial.id.clear();
        initial.builtin = false;
        initial.plugin.clear();
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
    if (!t || t->builtin || isPluginTheme(*t)) return;
    if (QMessageBox::question(this, tr("Delete theme"), tr("Delete the theme \"%1\"?").arg(t->name)) != QMessageBox::Yes) return;
    const bool wasCurrent = id == currentThemeId();
    removeCustomTheme(id);
    if (wasCurrent) window_.setThemeId(QString::fromLatin1(kDefaultThemeId));
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

    confirmMerge_ = new QCheckBox(tr("Confirm merges that fill unselected space"));
    confirmMerge_->setObjectName(QStringLiteral("settingsConfirmNonConvexMerge"));
    confirmMerge_->setChecked(window_.editorSession().confirmNonConvexMerge);
    form->addRow(tr("Editing"), confirmMerge_);
    pivotAtOrigin_ = new QCheckBox(tr("Rotate a single object about its origin"));
    pivotAtOrigin_->setObjectName(QStringLiteral("settingsPivotAtObjectOrigin"));
    pivotAtOrigin_->setChecked(window_.editorSession().pivotAtObjectOrigin());
    pivotAtOrigin_->setToolTip(tr("Like Hammer, a lone selected entity rotates and scales about its origin. Off pivots "
                                  "on the centre of its bounds. Applies while the transform pivot is Bounds centre."));
    form->addRow(QString(), pivotAtOrigin_);
    modelEdgeOverlay_ = new QCheckBox(tr("Draw model wireframe overlay in Flat view"));
    modelEdgeOverlay_->setObjectName(QStringLiteral("settingsModelEdgeOverlay"));
    modelEdgeOverlay_->setChecked(window_.editorSession().modelEdgeOverlay());
    modelEdgeOverlay_->setToolTip(tr("Triangle edges over models in the Flat display mode and on selected models. "
                                     "The Wireframe mode always shows them. Also on the Transform toolbar."));
    form->addRow(tr("Models"), modelEdgeOverlay_);
    entityIconsNearest_ = new QCheckBox(tr("Turn off texture filtering on entity icons"));
    entityIconsNearest_->setObjectName(QStringLiteral("settingsEntityIconsNearest"));
    entityIconsNearest_->setChecked(window_.editorSession().entityIconsNearest());
    entityIconsNearest_->setToolTip(tr("Draws the billboarded icons of entities with nearest-neighbour sampling, so "
                                       "their pixels stay sharp instead of blurring when scaled."));
    form->addRow(tr("Entity icons"), entityIconsNearest_);

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

    prefabFolder_ = new QLineEdit(QSettings().value(QStringLiteral("prefabs/folder")).toString());
    prefabFolder_->setObjectName(QStringLiteral("settingsPrefabFolder"));
    prefabFolder_->setPlaceholderText(tr("Default: the game's prefabs folder"));
    prefabFolder_->setToolTip(tr("Where Create Prefab saves and the Prefabs tab lists. Empty uses the active game "
                                 "profile's own prefabs folder."));
    auto* prefabBrowse = new QPushButton(tr("Browse…"));
    prefabBrowse->setObjectName(QStringLiteral("settingsPrefabFolderBrowse"));
    connect(prefabBrowse, &QPushButton::clicked, this, [this] {
        const auto dir = QFileDialog::getExistingDirectory(this, tr("Prefab folder"), prefabFolder_->text());
        if (!dir.isEmpty()) prefabFolder_->setText(dir);
    });
    auto* prefabRow = new QHBoxLayout;
    prefabRow->addWidget(prefabFolder_, 1);
    prefabRow->addWidget(prefabBrowse);
    form->addRow(tr("Prefab folder"), prefabRow);

    inspectorOpens_ = new QComboBox;
    inspectorOpens_->setObjectName(QStringLiteral("settingsInspectorOpens"));
    inspectorOpens_->addItem(tr("Workspace area"), false);
    inspectorOpens_->addItem(tr("Floating window"), true);
    inspectorOpens_->setCurrentIndex(MainWindow::inspectorInWindow() ? 1 : 0);
    inspectorOpens_->setToolTip(tr("Where Object properties shows the inspector. Without an inspector area in the "
                                   "workspace it always opens a window."));
    form->addRow(tr("Object properties opens in"), inspectorOpens_);
    facePropertiesOpens_ = new QComboBox;
    facePropertiesOpens_->setObjectName(QStringLiteral("settingsFacePropertiesOpens"));
    facePropertiesOpens_->addItem(tr("Workspace area"), false);
    facePropertiesOpens_->addItem(tr("Floating window"), true);
    facePropertiesOpens_->setCurrentIndex(MainWindow::facePropertiesInWindow() ? 1 : 0);
    facePropertiesOpens_->setToolTip(tr("Where Face Properties shows when the Face edit tool is activated. "
                                        "Without a Face Properties area it opens a floating window."));
    form->addRow(tr("Face Properties opens in"), facePropertiesOpens_);
    compilationOpens_ = new QComboBox;
    compilationOpens_->setObjectName(QStringLiteral("settingsCompilationOpens"));
    compilationOpens_->addItem(tr("Workspace area"), false);
    compilationOpens_->addItem(tr("Floating window"), true);
    compilationOpens_->setCurrentIndex(MainWindow::compilationInWindow() ? 1 : 0);
    compilationOpens_->setToolTip(tr("Where compilation commands show the compilation screen. "
                                    "Without a Compilation area it opens a floating window."));
    form->addRow(tr("Compilation opens in"), compilationOpens_);
    materialBrowserOpens_ = new QComboBox;
    materialBrowserOpens_->setObjectName(QStringLiteral("settingsMaterialBrowserOpens"));
    materialBrowserOpens_->addItem(tr("Materials area"), false);
    materialBrowserOpens_->addItem(tr("Floating window"), true);
    materialBrowserOpens_->setCurrentIndex(MainWindow::materialBrowserInWindow() ? 1 : 0);
    form->addRow(tr("Material browser opens in"), materialBrowserOpens_);
    uvAlignment_ = new QComboBox;
    uvAlignment_->setObjectName(QStringLiteral("settingsUvAlignment"));
    uvAlignment_->addItem(tr("Face"), static_cast<int>(geometry::UvAlignment::Face));
    uvAlignment_->addItem(tr("World"), static_cast<int>(geometry::UvAlignment::World));
    uvAlignment_->setCurrentIndex(window_.editorSession().newSolidStyle().alignment == geometry::UvAlignment::World ? 1 : 0);
    form->addRow(tr("New solid UV alignment"), uvAlignment_);

    // The same dialogs as the viewport menu's Display mode submenu; each saves on its own OK.
    auto* lightingButtons = new QHBoxLayout;
    auto* bakeSettings = new QPushButton(tr("Bake settings…"));
    bakeSettings->setObjectName(QStringLiteral("settingsBakeSettings"));
    bakeSettings->setToolTip(tr("Apex's own lightmap bake, shown by the Baked and Hybrid display modes."));
    auto* vradSettings = new QPushButton(tr("VRAD settings…"));
    vradSettings->setObjectName(QStringLiteral("settingsVradSettings"));
    vradSettings->setToolTip(tr("The VRAD compile behind the VRAD and Hybrid display modes."));
    lightingButtons->addWidget(bakeSettings);
    lightingButtons->addWidget(vradSettings);
    lightingButtons->addStretch(1);
    form->addRow(tr("Lighting"), lightingButtons);
    connect(bakeSettings, &QPushButton::clicked, this, [this] { showBakeSettings(this); });
    connect(vradSettings, &QPushButton::clicked, this, [this] { showVradSettings(this); });
    return page;
}

QWidget* SettingsDialog::buildInputPage() {
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    originalInput_ = window_.editorSession().pieSettings();
    const auto pixels = [&](const char* name, double minimum, double maximum, double value) {
        auto* box = new QDoubleSpinBox;
        box->setObjectName(QString::fromLatin1(name));
        box->setRange(minimum, maximum);
        box->setSuffix(tr(" px"));
        box->setValue(value);
        return box;
    };
    pieRadius_ = pixels("settingsPieRadius", 20, 500, originalInput_.radius);
    pieDeadZone_ = pixels("settingsPieDeadZone", 0, 500, originalInput_.deadZone);
    pieConfirm_ = pixels("settingsPieConfirmOnStopDistance", 0, 2000, originalInput_.confirmOnStopDistance);
    pieConfirm_->setSpecialValueText(tr("Off"));
    const auto milliseconds = [&](const char* name, std::chrono::milliseconds value) {
        auto* box = new QSpinBox;
        box->setObjectName(QString::fromLatin1(name));
        box->setRange(0, 5000);
        box->setSuffix(tr(" ms"));
        box->setValue(static_cast<int>(value.count()));
        return box;
    };
    pieTap_ = milliseconds("settingsPieTapTimeout", originalInput_.tapTimeout);
    pieAnimation_ = milliseconds("settingsPieAnimation", originalInput_.animation);
    form->addRow(tr("Pie radius"), pieRadius_);
    form->addRow(tr("Dead zone"), pieDeadZone_);
    form->addRow(tr("Tap timeout"), pieTap_);
    form->addRow(tr("Open animation (0 = instant)"), pieAnimation_);
    form->addRow(tr("Confirm-on-stop distance (0 = off)"), pieConfirm_);
    return page;
}

editor::PieSettings SettingsDialog::inputSettings() const {
    auto settings = originalInput_;
    settings.radius = pieRadius_->value();
    settings.deadZone = pieDeadZone_->value();
    settings.tapTimeout = std::chrono::milliseconds(pieTap_->value());
    settings.animation = std::chrono::milliseconds(pieAnimation_->value());
    settings.confirmOnStopDistance = pieConfirm_->value();
    return settings;
}
bool SettingsDialog::inputModified() const {
    const auto settings = inputSettings();
    return settings.radius != originalInput_.radius || settings.deadZone != originalInput_.deadZone ||
           settings.tapTimeout != originalInput_.tapTimeout || settings.animation != originalInput_.animation ||
           settings.confirmOnStopDistance != originalInput_.confirmOnStopDistance;
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

// ---- Pies page (plan 0.9 D7) ---------------------------------------------------------------

namespace {
// Two layouts are the same when they put the same action and label in the same slots.
bool samePie(const editor::PieMenu& a, const editor::PieMenu& b) {
    if (a.title != b.title || a.items.size() != b.items.size()) return false;
    const auto key = [](const editor::PieMenu& menu) {
        std::vector<std::tuple<int, std::string, std::string>> keys;
        const auto placed = editor::layoutSlots(menu);
        for (std::size_t i = 0; i < menu.items.size(); ++i)
            keys.emplace_back(static_cast<int>(placed[i]), menu.items[i].action, menu.items[i].label);
        std::ranges::sort(keys);
        return keys;
    };
    return key(a) == key(b);
}
}  // namespace

QWidget* SettingsDialog::buildPiesPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Pie:")));
    piePicker_ = new QComboBox;
    piePicker_->setObjectName(QStringLiteral("piePicker"));
    piePicker_->setMinimumWidth(200);
    top->addWidget(piePicker_);
    pieStatus_ = new QLabel;
    pieStatus_->setObjectName(QStringLiteral("pieStatus"));
    top->addWidget(pieStatus_);
    top->addStretch(1);
    pieReset_ = new QPushButton(tr("Reset to default"));
    pieReset_->setObjectName(QStringLiteral("pieReset"));
    pieReset_->setToolTip(tr("Drop this preset's layout and use the built-in one."));
    top->addWidget(pieReset_);
    layout->addLayout(top);
    auto* hint = new QLabel(tr("A pie layout is stored in the keybinding preset. Changes are saved with it: use Save or Save As on the "
                               "Keybindings page, or OK. An action from a plugin that is not loaded stays in the layout, hidden "
                               "from the pie until the plugin is back."));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* form = new QFormLayout;
    pieTitle_ = new QLineEdit;
    pieTitle_->setObjectName(QStringLiteral("pieTitle"));
    form->addRow(tr("Title"), pieTitle_);
    layout->addLayout(form);

    const QStringList names{tr("Up (N)"), tr("Up right (NE)"), tr("Right (E)"), tr("Down right (SE)"),
                            tr("Down (S)"), tr("Down left (SW)"), tr("Left (W)"), tr("Up left (NW)")};
    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(2, 1);
    for (int i = 0; i < 8; ++i) {
        auto& row = pieRows_[static_cast<std::size_t>(i)];
        row.action = new QComboBox;
        row.action->setObjectName(QStringLiteral("pieAction%1").arg(i));
        row.label = new QLineEdit;
        row.label->setObjectName(QStringLiteral("pieLabel%1").arg(i));
        row.label->setPlaceholderText(tr("Label"));
        row.note = new QLabel;
        row.note->setObjectName(QStringLiteral("pieNote%1").arg(i));
        grid->addWidget(new QLabel(names[i]), i, 0);
        grid->addWidget(row.action, i, 1);
        grid->addWidget(row.label, i, 2);
        grid->addWidget(row.note, i, 3);
        connect(row.action, &QComboBox::currentIndexChanged, this, [this, i] {
            if (pieLoading_) return;
            auto& r = pieRows_[static_cast<std::size_t>(i)];
            // A label that was never typed by hand follows the action's name.
            const auto* action = editor::actionRegistry().find(r.action->currentData().toString().toStdString());
            const QString automatic = r.label->property("auto").toString();
            if (r.label->text().isEmpty() || r.label->text() == automatic) {
                const QString name = action ? fromStd(action->name) : QString();
                r.label->setText(name);
                r.label->setProperty("auto", name);
            }
            r.label->setEnabled(!r.action->currentData().toString().isEmpty());
            pieEdited();
        });
        connect(row.label, &QLineEdit::textEdited, this, [this] { pieEdited(); });
    }
    layout->addLayout(grid);
    layout->addStretch(1);

    connect(piePicker_, &QComboBox::currentIndexChanged, this, [this] { loadPieRows(); });
    connect(pieTitle_, &QLineEdit::textEdited, this, [this] { pieEdited(); });
    connect(pieReset_, &QPushButton::clicked, this, [this] { resetPie(); });
    return page;
}

editor::ActionId SettingsDialog::selectedPie() const {
    return piePicker_ ? piePicker_->currentData().toString().toStdString() : editor::ActionId{};
}

void SettingsDialog::refreshPies() {
    if (!piePicker_) return;
    const auto keep = selectedPie();
    {
        const QSignalBlocker block(piePicker_);
        piePicker_->clear();
        for (const auto& [id, menu] : editor::pieRegistry().entries()) {
            const auto* action = editor::actionRegistry().find(id);
            piePicker_->addItem(action ? fromStd(action->name) : fromStd(menu.title), fromStd(id));
        }
        const int i = piePicker_->findData(fromStd(keep));
        piePicker_->setCurrentIndex(i < 0 ? 0 : i);
    }
    loadPieRows();
}

void SettingsDialog::loadPieRows() {
    if (!piePicker_) return;
    const auto id = selectedPie();
    const bool hasPie = !id.empty();
    pieLoading_ = true;
    const auto menu = hasPie ? editor::effectivePie(id, working_) : editor::PieMenu{};
    pieTitle_->setEnabled(hasPie);
    pieTitle_->setText(fromStd(menu.title));
    std::array<const editor::PieItem*, 8> bySlot{};
    const auto placed = editor::layoutSlots(menu);
    for (std::size_t i = 0; i < menu.items.size() && i < placed.size(); ++i) bySlot[static_cast<std::size_t>(placed[i])] = &menu.items[i];
    const QBrush dim = palette().brush(QPalette::Disabled, QPalette::Text);
    for (std::size_t i = 0; i < 8; ++i) {
        auto& row = pieRows_[i];
        const QSignalBlocker block(row.action);
        row.action->clear();
        row.action->addItem(tr("(empty)"), QString());
        for (const auto& action : editor::actionRegistry().entries()) {
            row.action->addItem(fromStd(action.name), fromStd(action.id));
            row.action->setItemData(row.action->count() - 1, fromStd(action.id), Qt::ToolTipRole);
        }
        const auto* item = bySlot[i];
        QString note;
        int index = 0;
        if (item) {
            const QString action = fromStd(item->action);
            index = row.action->findData(action);
            if (index < 0) {
                // Dormant: kept in the layout, hidden from the pie while the plugin is absent.
                row.action->addItem(tr("%1 (plugin not loaded)").arg(action), action);
                index = row.action->count() - 1;
                row.action->setItemData(index, dim, Qt::ForegroundRole);
                note = tr("Hidden until the plugin is loaded");
            }
        }
        row.action->setCurrentIndex(index);
        row.label->setText(item ? fromStd(item->label) : QString());
        row.label->setProperty("auto", QString());
        row.label->setEnabled(item != nullptr);
        row.action->setEnabled(hasPie);
        row.note->setText(note);
    }
    pieLoading_ = false;
    const bool own = hasPie && editor::hasOwnPie(working_, id);
    pieStatus_->setText(own ? tr("Custom layout in this preset") : tr("Built-in layout"));
    pieReset_->setEnabled(own);
}

editor::PieMenu SettingsDialog::pieFromRows() const {
    editor::PieMenu menu;
    menu.title = pieTitle_->text().toStdString();
    for (std::size_t i = 0; i < 8; ++i) {
        const auto& row = pieRows_[i];
        const auto action = row.action->currentData().toString().toStdString();
        if (action.empty()) continue;
        std::string label = row.label->text().toStdString();
        if (label.empty()) {
            const auto* entry = editor::actionRegistry().find(action);
            label = entry ? entry->name : action;
        }
        menu.items.push_back({action, static_cast<editor::PieSlot>(i), std::move(label)});
    }
    return menu;
}

void SettingsDialog::pieEdited() {
    if (pieLoading_) return;
    const auto id = selectedPie();
    if (id.empty()) return;
    auto menu = pieFromRows();
    const auto* builtin = editor::pieRegistry().find(id);
    // A layout equal to the built-in one is stored as "none", so presets stay minimal.
    if (builtin && samePie(menu, *builtin)) working_.pies.erase(id);
    else working_.pies[id] = std::move(menu);
    const bool own = editor::hasOwnPie(working_, id);
    pieStatus_->setText(own ? tr("Custom layout in this preset") : tr("Built-in layout"));
    pieReset_->setEnabled(own);
    refreshButtons();
}

void SettingsDialog::resetPie() {
    const auto id = selectedPie();
    if (id.empty()) return;
    working_.pies.erase(id);
    loadPieRows();
    refreshButtons();
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
    refreshPies();
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

namespace {
const editor::Binding& bindingOrDefault(const editor::Preset& preset, const editor::ActionId& id) {
    const auto it = preset.bindings.find(id);
    return it == preset.bindings.end() ? editor::defaultBinding(id) : it->second;
}
}

QString SettingsDialog::pluginNameFor(std::string_view id) const {
    if (!id.starts_with("plugin.")) return {};
    const plugin_host::Plugin* best = nullptr;
    if (const auto* host = window_.pluginHost()) {
        for (const auto& plugin : host->plugins())
            if (plugin_host::inNamespace(plugin.id, id) && (!best || plugin.id.size() > best->id.size())) best = &plugin;
    }
    if (best) return fromStd(best->name.empty() ? best->id : best->name);
    // The plugin is not installed: its id is the second dotted component of "plugin.<id>.<name>".
    const auto rest = id.substr(7);
    const auto dot = rest.rfind('.');
    return fromStd(dot == std::string_view::npos ? rest : rest.substr(0, dot));
}

namespace {
// What the When column shows for a tool-scoped binding: the tool's name, not its id.
QString contextText(const editor::BindingContext& context) {
    if (context.toolId.empty()) return {};
    if (const auto* tool = editor::actionRegistry().find(context.toolId))
        return fromStd("While " + tool->name + " is active");
    return fromStd("While " + context.toolId + " is active");
}
}  // namespace

void SettingsDialog::refreshTree() {
    const QString query = filter_ ? filter_->text().trimmed() : QString();
    const auto selected = selectedAction();
    // Plugin actions a preset does not bind may still carry the plugin's suggestion for it (docs/plans/0.9.md
    // D1). Those are live, so the page shows them as suggestions, never as "not set".
    suggestions_.clear();
    suggestionConflicts_.clear();
    try {
        const editor::Keymap keymap(working_);
        suggestions_ = keymap.suggestions();
        suggestionConflicts_ = keymap.suggestionConflicts();
    } catch (const std::exception& e) {
        log::warn("keybinding page: {}", e.what());
    }
    QSignalBlocker block(tree_);
    tree_->clear();
    std::map<std::string, QTreeWidgetItem*> categories;
    const auto categoryItem = [&](const std::string& name, bool dim) {
        auto*& parent = categories[name];
        if (!parent) {
            parent = new QTreeWidgetItem(tree_, QStringList{fromStd(name)});
            QFont font = parent->font(0);
            font.setBold(true);
            parent->setFont(0, font);
            parent->setFlags(Qt::ItemIsEnabled);
            if (dim) parent->setForeground(0, tree_->palette().color(QPalette::Disabled, QPalette::Text));
        }
        return parent;
    };
    for (const auto& action : editor::actionRegistry().entries()) {
        const auto& binding = bindingOrDefault(working_, action.id);
        const bool bound = working_.bindings.contains(action.id);
        const auto suggestion = suggestions_.find(action.id);
        const bool suggested = !bound && suggestion != suggestions_.end();
        QString keys = chordsText(binding);
        QString when = contextText(binding.context);
        if (suggested) {
            keys = tr("%1 (suggested)").arg(fromStd(suggestion->second.chord.toString()));
            when = contextText(suggestion->second.context);
        }
        const QString plugin = pluginNameFor(action.id);
        const std::string category = plugin.isEmpty() ? action.category : ("Plugin: " + plugin.toStdString());
        if (!query.isEmpty() && !fromStd(action.name).contains(query, Qt::CaseInsensitive) &&
            !fromStd(category).contains(query, Qt::CaseInsensitive) &&
            !keys.contains(query, Qt::CaseInsensitive))
            continue;
        auto* item = new QTreeWidgetItem(categoryItem(category, false));
        item->setData(0, kIdRole, fromStd(action.id));
        item->setText(0, fromStd(action.name));
        item->setText(1, keys.isEmpty() ? QStringLiteral("—") : keys);
        item->setText(2, when);
        if (suggested) {
            QFont font = item->font(1);
            font.setItalic(true);
            item->setFont(1, font);
            item->setToolTip(1, tr("Suggested by the plugin for this preset. It is active; set or clear the key to override it."));
        } else if (const auto conflict = suggestionConflicts_.find(action.id); !bound && conflict != suggestionConflicts_.end()) {
            item->setToolTip(1, tr("The plugin's suggested key is not used: %1").arg(fromStd(conflict->second)));
        }
        const auto* original = original_.bindings.contains(action.id) ? &original_.bindings.at(action.id) : nullptr;
        if (original && !sameBinding(*original, binding)) {
            QFont font = item->font(1);
            font.setItalic(true);
            font.setBold(true);
            item->setFont(1, font);
        }
        if (fromStd(action.id) == fromStd(selected)) tree_->setCurrentItem(item);
    }
    // Bindings for plugin actions whose plugin is absent or disabled stay in the preset (docs/plans/0.9.md
    // D1). They are listed greyed, so the user can see them, and cannot be edited until the plugin is back.
    for (const auto& [id, binding] : working_.bindings) {
        if (!id.starts_with("plugin.") || editor::actionRegistry().find(id)) continue;
        const QString plugin = pluginNameFor(id);
        const QString keys = chordsText(binding);
        const std::string category = "Plugin: " + plugin.toStdString() + " (not loaded)";
        if (!query.isEmpty() && !fromStd(id).contains(query, Qt::CaseInsensitive) &&
            !fromStd(category).contains(query, Qt::CaseInsensitive) && !keys.contains(query, Qt::CaseInsensitive))
            continue;
        auto* item = new QTreeWidgetItem(categoryItem(category, true));
        item->setText(0, fromStd(id));
        item->setText(1, keys.isEmpty() ? QStringLiteral("—") : keys);
        item->setText(2, contextText(binding.context));
        item->setFlags(Qt::NoItemFlags);  // Not selectable: nothing to edit while the plugin is absent.
        item->setToolTip(0, tr("The plugin is not loaded. This binding is kept in the preset and applies again when it is."));
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
    const auto& binding = bindingOrDefault(working_, id);
    const bool suggested = !working_.bindings.contains(id) && suggestions_.contains(id);
    clearButton_->setEnabled(suggested || !std::holds_alternative<editor::Unbound>(binding.value));
    resetButton_->setEnabled(!sameBinding(binding, bindingOrDefault(original_, id)));
    QString text = QStringLiteral("<b>%1</b><br>%2").arg(fromStd(action->name).toHtmlEscaped(),
                                                       fromStd(action->description).toHtmlEscaped());
    if (const QString plugin = pluginNameFor(id); !plugin.isEmpty())
        text += QStringLiteral("<br>%1").arg(tr("Plugin: %1").arg(plugin.toHtmlEscaped()));
    if (suggested) {
        const auto& s = suggestions_.at(id);
        text += QStringLiteral("<br><i>%1</i>")
                    .arg(tr("Suggested key %1 from the plugin; it is active in this preset until you set or clear it.")
                             .arg(fromStd(s.chord.toString()).toHtmlEscaped()));
    } else if (const auto conflict = suggestionConflicts_.find(id);
               !working_.bindings.contains(id) && conflict != suggestionConflicts_.end()) {
        text += QStringLiteral("<br><i>%1</i>").arg(tr("The plugin's suggested key is not used: %1").arg(fromStd(conflict->second).toHtmlEscaped()));
    } else if (const auto* unbound = std::get_if<editor::Unbound>(&binding.value))
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
        if (const auto* keys = std::get_if<std::vector<editor::KeyChord>>(&bindingOrDefault(working_, id).value))
            chords = *keys;
        else if (const auto suggestion = suggestions_.find(id); suggestion != suggestions_.end() && !working_.bindings.contains(id))
            chords.push_back(suggestion->second.chord);
    }
    if (std::ranges::find(chords, chord) == chords.end()) chords.push_back(chord);
    if (const auto owner = editor::chordOwner(working_, chord, id, bindingOrDefault(working_, id).context)) {
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
    if (!original_.bindings.contains(id)) {
        working_.bindings.erase(id);
        refreshTree();
        return;
    }
    const auto& base = bindingOrDefault(original_, id);
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
    if (!keysModified() && !inputModified()) return true;
    const auto answer = QMessageBox::question(this, tr("Unapplied preferences"),
                                              tr("Discard unapplied preferences?"));
    return answer == QMessageBox::Yes;
}

bool SettingsDialog::applyAll() {
    const auto settings = inputSettings();
    window_.editorSession().setPieSettings(settings);
    savePieSettings(settings);
    originalInput_ = settings;
    window_.editorSession().confirmNonConvexMerge = confirmMerge_->isChecked();
    QSettings().setValue("editing/confirmNonConvexMerge", confirmMerge_->isChecked());
    window_.editorSession().setPivotAtObjectOrigin(pivotAtOrigin_->isChecked());
    QSettings().setValue("editing/pivotAtObjectOrigin", pivotAtOrigin_->isChecked());
    window_.editorSession().setModelEdgeOverlay(modelEdgeOverlay_->isChecked());
    QSettings().setValue("view/modelEdgeOverlay", modelEdgeOverlay_->isChecked());
    window_.editorSession().setEntityIconsNearest(entityIconsNearest_->isChecked());
    QSettings().setValue("view/entityIconsNearest", entityIconsNearest_->isChecked());
    window_.setNewSolidStyle(window_.editorSession().newSolidStyle().material,
                             static_cast<geometry::UvAlignment>(uvAlignment_->currentData().toInt()));
    if (theme_->currentData().toString() != currentThemeId()) window_.setThemeId(theme_->currentData().toString());
    window_.setAutosave(autosave_->isChecked(), autosaveMinutes_->value());
    QSettings().setValue(QStringLiteral("prefabs/folder"), prefabFolder_->text().trimmed());
    window_.gameContent().setPrefabUserFolder(std::filesystem::path(prefabFolder_->text().trimmed().toStdString()));
    MainWindow::setInspectorInWindow(inspectorOpens_->currentData().toBool());
    MainWindow::setFacePropertiesInWindow(facePropertiesOpens_->currentData().toBool());
    MainWindow::setCompilationInWindow(compilationOpens_->currentData().toBool());
    MainWindow::setMaterialBrowserInWindow(materialBrowserOpens_->currentData().toBool());
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
