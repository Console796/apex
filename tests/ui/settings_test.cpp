// Headless tests for the Preferences dialog and keybinding editor.
#include "test_app.hpp"
#include <doctest/doctest.h>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <utility>

#include "editor/pie_menus.hpp"
#include "editor/preset_io.hpp"
#include "ui/bake_controller.hpp"
#include "ui/main_window.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/theme.hpp"

namespace {
using namespace apex;

void press(QWidget& w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent down(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(&w, &down);
    QKeyEvent up(QEvent::KeyRelease, key, mods);
    QCoreApplication::sendEvent(&w, &up);
}

QTreeWidgetItem* rowFor(QTreeWidget& tree, const QString& id) {
    QTreeWidgetItemIterator it(&tree);
    for (; *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == id) return *it;
    }
    return nullptr;
}
}  // namespace

TEST_CASE("key capture records chords and held modifiers") {
    ui_test::app();
    {
        ui::KeyCaptureDialog dialog("Test", false);
        dialog.show();
        press(dialog, Qt::Key_Control);  // Bare modifier is ignored when not allowed.
        CHECK_FALSE(dialog.chord());
        press(dialog, Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);
        REQUIRE(dialog.chord());
        CHECK(dialog.chord()->toString() == "Ctrl+Shift+K");
        CHECK(dialog.result() == QDialog::Accepted);
    }
    {
        ui::KeyCaptureDialog dialog("Snap", true);
        dialog.show();
        press(dialog, Qt::Key_Alt);
        REQUIRE(dialog.chord());
        CHECK(dialog.chord()->toString() == "Alt");
    }
    {
        ui::KeyCaptureDialog dialog("Esc", false);
        dialog.show();
        press(dialog, Qt::Key_Escape);  // Escape is a bindable key, not a cancel.
        REQUIRE(dialog.chord());
        CHECK(dialog.chord()->key == "Escape");
    }
}

TEST_CASE("preferences edit and apply general settings and a saved keybinding preset") {
    ui_test::app();
    QSettings().setValue("editor/preset", "Hammer");
    ui::MainWindow window;
    window.show();
    const auto before = window.presetStore().list();

    ui::SettingsDialog dialog(window);
    CHECK(dialog.workingPreset().name == "Hammer");
    CHECK_FALSE(dialog.keysModified());

    auto* tree = dialog.findChild<QTreeWidget*>("keyTree");
    REQUIRE(tree);
    auto* row = rowFor(*tree, "edit.undo");
    REQUIRE(row);
    tree->setCurrentItem(row);
    CHECK(row->text(1) == "Ctrl+Z");
    auto* filter = dialog.findChild<QWidget*>("keyFilter");
    REQUIRE(filter);

    // Editing a built-in preset marks it modified but Save is disabled until it is renamed.
    auto* save = dialog.findChild<QPushButton*>("keySave");
    auto* del = dialog.findChild<QPushButton*>("keyDelete");
    CHECK_FALSE(del->isEnabled());

    // Drive the same edit path the buttons use.
    auto preset = dialog.workingPreset();
    editor::assignChords(preset, "edit.undo", {editor::KeyChord::parse("Ctrl+Shift+U").value()});
    CHECK(editor::validate(preset).empty());
    CHECK_FALSE(save->isEnabled());

    // The Settings menu lists the built-ins; saving user presets adds them.
    preset.name = "Test Keys";
    REQUIRE(window.presetStore().save(preset));
    window.refreshPresetMenu();
    REQUIRE(window.findChild<QAction*>("preset.Test Keys"));
    window.findChild<QAction*>("preset.Test Keys")->trigger();
    CHECK(window.editorSession().preset().name == "Test Keys");
    CHECK(QSettings().value("editor/preset").toString() == "Test Keys");
    CHECK(window.editorSession().keymap().lookup(editor::KeyChord::parse("Ctrl+Shift+U").value()) == "edit.undo");

    // Reopen: the dialog starts from the active user preset and allows saving and deleting it.
    ui::SettingsDialog second(window);
    CHECK(second.workingPreset().name == "Test Keys");
    CHECK(second.findChild<QPushButton*>("keyDelete")->isEnabled());
    CHECK(second.findChild<QComboBox*>("keyPresetCombo")->findData("Test Keys") >= 0);

    // A fresh window restores the user preset from the store.
    ui::MainWindow reopened;
    CHECK(reopened.editorSession().preset().name == "Test Keys");

    REQUIRE(window.presetStore().remove("Test Keys"));
    QSettings().setValue("editor/preset", "Hammer");
    window.refreshPresetMenu();
    CHECK(window.findChild<QAction*>("preset.Test Keys") == nullptr);
    CHECK(window.presetStore().list() == before);
}

TEST_CASE("active material and UV alignment persist without a new solid material setting") {
    ui_test::app();
    QSettings settings;
    const auto oldMaterial = settings.value("editor/newSolidMaterial");
    const auto oldWorld = settings.value("editor/newSolidWorldUv");
    const auto oldBrowser = settings.value("ui/materialBrowserWindow");
    const auto oldFaceWindow = settings.value("ui/facePropertiesWindow");
    const auto oldCompilationWindow = settings.value("ui/compilationWindow");
    const auto oldExtrapolation = settings.value("editor/faceExtrapolation");
    settings.setValue("editor/newSolidMaterial", "test/loaded");
    settings.setValue("editor/newSolidWorldUv", true);
    settings.setValue("editor/faceExtrapolation", static_cast<int>(editor::FaceExtrapolation::Match));
    {
        ui::MainWindow window;
        CHECK(window.editorSession().newSolidStyle().material == "test/loaded");
        CHECK(window.editorSession().newSolidStyle().alignment == geometry::UvAlignment::World);
        CHECK(window.editorSession().faceExtrapolation() == editor::FaceExtrapolation::Match);
        auto* toggle = window.findChild<QAction*>("newSolidWorldUv");
        REQUIRE(toggle);
        CHECK(toggle->isChecked());
        toggle->trigger();
        CHECK(window.editorSession().newSolidStyle().alignment == geometry::UvAlignment::Face);
        CHECK_FALSE(settings.value("editor/newSolidWorldUv").toBool());
        ui::SettingsDialog dialog(window);
        auto* material = dialog.findChild<QLineEdit*>("settingsNewSolidMaterial");
        auto* alignment = dialog.findChild<QComboBox*>("settingsUvAlignment");
        auto* browser = dialog.findChild<QComboBox*>("settingsMaterialBrowserOpens");
        auto* faceProperties = dialog.findChild<QComboBox*>("settingsFacePropertiesOpens");
        auto* compilation = dialog.findChild<QComboBox*>("settingsCompilationOpens");
        REQUIRE(compilation);
        compilation->setCurrentIndex(1);
        CHECK_FALSE(material);
        REQUIRE(alignment);
        REQUIRE(browser);
        REQUIRE(faceProperties);
        browser->setCurrentIndex(1);
        faceProperties->setCurrentIndex(1);
        window.editorSession().cycleFaceExtrapolation();
        window.activateMaterial("test/changed");
        alignment->setCurrentIndex(1);
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        REQUIRE(buttons);
        buttons->button(QDialogButtonBox::Apply)->click();
        CHECK(window.editorSession().newSolidStyle().material == "test/changed");
        CHECK(ui::MainWindow::materialBrowserInWindow());
        CHECK(ui::MainWindow::facePropertiesInWindow());
        CHECK(ui::MainWindow::compilationInWindow());
        CHECK(toggle->isChecked());
    }
    {
        ui::MainWindow window;
        CHECK(window.editorSession().newSolidStyle().material == "test/changed");
        CHECK(window.editorSession().newSolidStyle().alignment == geometry::UvAlignment::World);
        CHECK(window.editorSession().faceExtrapolation() == editor::FaceExtrapolation::Wrap);
        window.newFile();
        auto* action = window.findChild<QAction*>("tool.block");
        REQUIRE(action);
        action->trigger();
        CHECK(window.editorSession().activeTool() == "tool.block");
        CHECK(action->isChecked());
    }
    if (oldMaterial.isValid()) settings.setValue("editor/newSolidMaterial", oldMaterial);
    else settings.remove("editor/newSolidMaterial");
    if (oldWorld.isValid()) settings.setValue("editor/newSolidWorldUv", oldWorld);
    else settings.remove("editor/newSolidWorldUv");
    if (oldBrowser.isValid()) settings.setValue("ui/materialBrowserWindow", oldBrowser);
    else settings.remove("ui/materialBrowserWindow");
    if (oldFaceWindow.isValid()) settings.setValue("ui/facePropertiesWindow", oldFaceWindow);
    else settings.remove("ui/facePropertiesWindow");
    if (oldCompilationWindow.isValid()) settings.setValue("ui/compilationWindow", oldCompilationWindow);
    else settings.remove("ui/compilationWindow");
    if (oldExtrapolation.isValid()) settings.setValue("editor/faceExtrapolation", oldExtrapolation);
    else settings.remove("editor/faceExtrapolation");
}

TEST_CASE("preferences persist the object-origin pivot and model overlay toggles") {
    apex::ui_test::app();
    QSettings().remove("editing/pivotAtObjectOrigin");
    QSettings().remove("view/modelEdgeOverlay");
    apex::ui::MainWindow window;
    auto& session = window.editorSession();
    CHECK(session.pivotAtObjectOrigin());
    CHECK(session.modelEdgeOverlay());
    {
        apex::ui::SettingsDialog prefs(window);
        auto* pivot = prefs.findChild<QCheckBox*>("settingsPivotAtObjectOrigin");
        auto* overlay = prefs.findChild<QCheckBox*>("settingsModelEdgeOverlay");
        REQUIRE(pivot);
        REQUIRE(overlay);
        CHECK(pivot->isChecked());
        CHECK(overlay->isChecked());
        pivot->setChecked(false);
        overlay->setChecked(false);
        prefs.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
    }
    CHECK_FALSE(session.pivotAtObjectOrigin());
    CHECK_FALSE(session.modelEdgeOverlay());
    CHECK_FALSE(QSettings().value("editing/pivotAtObjectOrigin", true).toBool());
    CHECK_FALSE(QSettings().value("view/modelEdgeOverlay", true).toBool());
    {
        apex::ui::MainWindow reopened;
        CHECK_FALSE(reopened.editorSession().pivotAtObjectOrigin());
        CHECK_FALSE(reopened.editorSession().modelEdgeOverlay());
    }
    QSettings().remove("editing/pivotAtObjectOrigin");
    QSettings().remove("view/modelEdgeOverlay");
}

TEST_CASE("bake settings store only values that differ from the defaults") {
    ui_test::app();
    QSettings settings;
    settings.remove("bake");
    settings.setValue("bake/bounces", 3);
    settings.setValue("bake/displayScale", 1.0);
    // Runs inside the modal loop: set bounces, then accept.
    const auto edit = [](int bounces) {
        QTimer::singleShot(0, [bounces] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialog);
            const auto spins = dialog->findChildren<QSpinBox*>();
            REQUIRE(spins.size() == 4);
            spins[0]->setValue(bounces); // Bounces, the first integer field.
            dialog->accept();
        });
        ui::showBakeSettings(nullptr);
    };
    edit(5);
    settings.sync();
    CHECK(settings.value("bake/bounces").toInt() == 5);
    CHECK_FALSE(settings.contains("bake/displayScale")); // Back at the default: removed.
    CHECK_FALSE(settings.contains("bake/maxPasses"));
    CHECK_FALSE(settings.contains("bake/rebakeReach"));
    CHECK_FALSE(settings.contains("bake/hybridBounceRing"));
    edit(int(lighting::BakeSessionOptions{}.baker.bounces));
    settings.sync();
    CHECK(settings.childGroups().indexOf("bake") < 0);
    CHECK(settings.allKeys().filter("bake/").isEmpty());
}

TEST_CASE("preferences open the bake and VRAD settings dialogs") {
    ui_test::app();
    ui::MainWindow window;
    ui::SettingsDialog prefs(window);
    for (const auto& [name, title] : {std::pair{"settingsBakeSettings", "Bake settings"},
                                      std::pair{"settingsVradSettings", "VRAD settings"}}) {
        CAPTURE(name);
        auto* button = prefs.findChild<QPushButton*>(name);
        REQUIRE(button);
        QString opened;
        // Runs inside the modal loop: note which dialog opened, then cancel it unchanged.
        QTimer::singleShot(0, [&opened] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(dialog);
            opened = dialog->windowTitle();
            dialog->reject();
        });
        button->click();
        CHECK(opened == QString::fromLatin1(title));
    }
}

TEST_CASE("preferences and shortcuts tolerate an unbound plugin action") {
    (void)apex::ui_test::app();
    auto& registry = apex::editor::mutableActionRegistry();
    struct Restore { apex::editor::ActionRegistry& target; apex::editor::ActionRegistry saved;
        ~Restore() { target = std::move(saved); } } restore{registry, registry};
    REQUIRE(registry.add({"plugin.preferences.example", "Plugin example", "Plugin", "Example action"}));
    apex::ui::MainWindow window;
    CHECK_NOTHROW(window.applyPreset(apex::editor::hammerPreset()));
    auto* pluginAction = window.findChild<QAction*>(QStringLiteral("plugin.preferences.example"));
    REQUIRE(pluginAction);
    CHECK(pluginAction->isEnabled());
    apex::ui::SettingsDialog dialog(window);
    CHECK_FALSE(dialog.workingPreset().bindings.contains("plugin.preferences.example"));
}

namespace {
struct ActionRegistryRestore {
    apex::editor::ActionRegistry& target = apex::editor::mutableActionRegistry();
    apex::editor::ActionRegistry saved = apex::editor::mutableActionRegistry();
    ~ActionRegistryRestore() { target = std::move(saved); }
};
QComboBox& combo(QDialog& dialog, const char* name) {
    auto* box = dialog.findChild<QComboBox*>(QString::fromLatin1(name));
    REQUIRE(box);
    return *box;
}
void choose(QComboBox& box, const char* actionId) {
    const int i = box.findData(QString::fromLatin1(actionId));
    REQUIRE(i >= 0);
    box.setCurrentIndex(i);
}
}  // namespace

TEST_CASE("the Pies page edits a layout, keeps it in the preset and resets to the built-in default") {
    ui_test::app();
    QSettings().setValue("editor/preset", "Hammer");
    ui::MainWindow window;
    ui::SettingsDialog dialog(window);
    CHECK(dialog.workingPreset().pies.empty());  // Today's built-in pies are the defaults.
    auto& picker = combo(dialog, "piePicker");
    REQUIRE(picker.findData("pie.tools") >= 0);
    REQUIRE(picker.findData("pie.objects") >= 0);
    picker.setCurrentIndex(picker.findData("pie.tools"));
    auto* status = dialog.findChild<QLabel*>("pieStatus");
    REQUIRE(status);
    CHECK(combo(dialog, "pieAction0").currentData().toString() == "tool.vertex");  // The default's N slot.
    CHECK(combo(dialog, "pieAction2").currentData().toString() == "tool.block");   // E.
    CHECK_FALSE(dialog.keysModified());
    CHECK_FALSE(dialog.findChild<QPushButton*>("pieReset")->isEnabled());

    // Put Undo in N and empty NE: the layout is now this preset's own.
    choose(combo(dialog, "pieAction0"), "edit.undo");
    combo(dialog, "pieAction1").setCurrentIndex(0);
    REQUIRE(dialog.workingPreset().pies.contains("pie.tools"));
    const auto& menu = dialog.workingPreset().pies.at("pie.tools");
    CHECK(menu.items.size() == 7);
    CHECK(editor::validatePie(menu).empty());
    REQUIRE(editor::pieItemAt(menu, editor::PieSlot::N));
    CHECK(editor::pieItemAt(menu, editor::PieSlot::N)->action == "edit.undo");
    CHECK_FALSE(editor::pieItemAt(menu, editor::PieSlot::NE));
    CHECK_FALSE(dialog.workingPreset().pies.contains("pie.objects"));
    CHECK(dialog.keysModified());
    CHECK(dialog.findChild<QPushButton*>("pieReset")->isEnabled());
    CHECK(status->text().contains("Custom"));

    // A hand-typed label sticks, and the edited preset survives serialization.
    dialog.findChild<QLineEdit*>("pieLabel0")->setText("Oops");
    emit dialog.findChild<QLineEdit*>("pieLabel0")->textEdited("Oops");
    CHECK(dialog.workingPreset().pies.at("pie.tools").items.size() == 7);
    const auto parsed = editor::parsePreset(editor::serializePreset(dialog.workingPreset()));
    REQUIRE(parsed);
    CHECK(editor::pieItemAt(parsed->pies.at("pie.tools"), editor::PieSlot::N)->label == "Oops");

    // Switching pies and back shows the edit, not the default.
    picker.setCurrentIndex(picker.findData("pie.objects"));
    CHECK(combo(dialog, "pieAction0").currentData().toString() == "edit.to_detail");
    picker.setCurrentIndex(picker.findData("pie.tools"));
    CHECK(combo(dialog, "pieAction0").currentData().toString() == "edit.undo");

    // Reset drops the entry; editing back to the default does too.
    dialog.findChild<QPushButton*>("pieReset")->click();
    CHECK(dialog.workingPreset().pies.empty());
    CHECK_FALSE(dialog.keysModified());
    CHECK(combo(dialog, "pieAction0").currentData().toString() == "tool.vertex");
    choose(combo(dialog, "pieAction0"), "edit.undo");
    REQUIRE(dialog.workingPreset().pies.contains("pie.tools"));
    choose(combo(dialog, "pieAction0"), "tool.vertex");
    dialog.findChild<QLineEdit*>("pieLabel0")->setText("Vertex");
    emit dialog.findChild<QLineEdit*>("pieLabel0")->textEdited("Vertex");
    CHECK(dialog.workingPreset().pies.empty());
}

TEST_CASE("an edited pie applies to the session and is saved with a user preset") {
    ui_test::app();
    QSettings().setValue("editor/preset", "Hammer");
    ui::MainWindow window;
    {
        ui::SettingsDialog dialog(window);
        combo(dialog, "piePicker").setCurrentIndex(combo(dialog, "piePicker").findData("pie.objects"));
        choose(combo(dialog, "pieAction0"), "edit.undo");  // N: replaces To Detail.
        REQUIRE(dialog.workingPreset().pies.contains("pie.objects"));
        auto preset = dialog.workingPreset();
        preset.name = "Pie Prefs";
        REQUIRE(window.presetStore().save(preset));
        window.applyPreset(preset);
    }
    CHECK(window.editorSession().preset().pies.contains("pie.objects"));
    CHECK(editor::pieItemAt(editor::effectivePie("pie.objects", window.editorSession().preset()), editor::PieSlot::N)->action == "edit.undo");
    const auto reloaded = window.presetStore().find("Pie Prefs");
    REQUIRE(reloaded);
    CHECK(reloaded->pies.contains("pie.objects"));
    CHECK(editor::pieItemAt(editor::effectivePie("pie.objects", *reloaded), editor::PieSlot::N)->action == "edit.undo");
    REQUIRE(window.presetStore().remove("Pie Prefs"));
    window.applyPreset(editor::hammerPreset());
    QSettings().setValue("editor/preset", "Hammer");
}

TEST_CASE("the Pies page keeps a dormant plugin item and shows it greyed") {
    ui_test::app();
    ActionRegistryRestore restore;
    ui::MainWindow window;
    auto preset = editor::hammerPreset();
    preset.name = "Dormant Pie";
    preset.pies["pie.tools"] = editor::PieMenu{"Mixed", {{"tool.block", editor::PieSlot::E, "Block"},
                                                         {"plugin.gone.act", editor::PieSlot::N, "Gone"}}};
    REQUIRE(window.presetStore().save(preset));
    window.applyPreset(preset);
    {
        ui::SettingsDialog dialog(window);
        REQUIRE(dialog.workingPreset().name == "Dormant Pie");
        auto& picker = combo(dialog, "piePicker");
        picker.setCurrentIndex(picker.findData("pie.tools"));
        auto& north = combo(dialog, "pieAction0");
        CHECK(north.currentData().toString() == "plugin.gone.act");
        CHECK(north.currentText().contains("not loaded"));
        CHECK(dialog.findChild<QLabel*>("pieNote0")->text().contains("Hidden"));
        // Editing another slot leaves the dormant item in the layout.
        choose(combo(dialog, "pieAction4"), "edit.undo");
        const auto& menu = dialog.workingPreset().pies.at("pie.tools");
        CHECK(menu.items.size() == 3);
        CHECK(editor::pieItemAt(menu, editor::PieSlot::N)->action == "plugin.gone.act");
        CHECK(editor::pieItemAt(menu, editor::PieSlot::N)->label == "Gone");
    }
    REQUIRE(window.presetStore().remove("Dormant Pie"));
    window.applyPreset(editor::hammerPreset());
    QSettings().setValue("editor/preset", "Hammer");
}

TEST_CASE("the Keybindings page shows plugin suggestions as suggestions and lists dormant bindings greyed") {
    ui_test::app();
    ActionRegistryRestore restore;
    auto& registry = editor::mutableActionRegistry();
    editor::Action fine{"plugin.listing.fine", "Fine", "Plugin", "Has a free suggested key."};
    fine.suggestedBindings.emplace("Hammer", "F24");
    REQUIRE(registry.add(std::move(fine)));
    editor::Action scoped{"plugin.listing.scoped", "Scoped", "Plugin", "Suggested only in the vertex tool."};
    scoped.suggestedBindings.emplace("Hammer", "F23");
    scoped.suggestedContexts.emplace("Hammer", "tool.vertex");
    REQUIRE(registry.add(std::move(scoped)));
    editor::Action clash{"plugin.listing.clash", "Clash", "Plugin", "Suggests a key a built-in action owns."};
    clash.suggestedBindings.emplace("Hammer", "Ctrl+Z");
    REQUIRE(registry.add(std::move(clash)));
    REQUIRE(registry.add({"plugin.listing.plain", "Plain", "Plugin", "No suggestion."}));

    QSettings().setValue("editor/preset", "Hammer");
    ui::MainWindow window;
    ui::SettingsDialog dialog(window);
    auto* tree = dialog.findChild<QTreeWidget*>("keyTree");
    REQUIRE(tree);
    // I-7: a suggested key used to read as "—" (unbound) here although it was live.
    auto* fineRow = rowFor(*tree, "plugin.listing.fine");
    REQUIRE(fineRow);
    CHECK(fineRow->text(1) == "F24 (suggested)");
    CHECK(fineRow->font(1).italic());
    CHECK(window.editorSession().keymap().lookup(editor::KeyChord::parse("F24").value()) == "plugin.listing.fine");
    auto* scopedRow = rowFor(*tree, "plugin.listing.scoped");
    REQUIRE(scopedRow);
    CHECK(scopedRow->text(1) == "F23 (suggested)");
    CHECK(scopedRow->text(2) == "While Vertex tool is active");
    auto* clashRow = rowFor(*tree, "plugin.listing.clash");
    REQUIRE(clashRow);
    CHECK(clashRow->text(1) == "—");  // Not used: edit.undo owns the key.
    CHECK(clashRow->toolTip(1).contains("edit.undo"));
    auto* plainRow = rowFor(*tree, "plugin.listing.plain");
    REQUIRE(plainRow);
    CHECK(plainRow->text(1) == "—");
    CHECK(fineRow->parent()->text(0).startsWith("Plugin: listing"));  // Grouped by plugin.
    CHECK(dialog.pluginNameFor("plugin.listing.fine") == "listing");
    CHECK(dialog.pluginNameFor("edit.undo").isEmpty());
    // Suggestions are not bound by the dialog: the preset is untouched.
    CHECK_FALSE(dialog.workingPreset().bindings.contains("plugin.listing.fine"));
    CHECK_FALSE(dialog.keysModified());

    // The detail line says so, and Clear is available to override the suggestion.
    tree->setCurrentItem(fineRow);
    auto* detail = dialog.findChild<QLabel*>("keyDetail");
    REQUIRE(detail);
    CHECK(detail->text().contains("Suggested key F24"));
    CHECK(dialog.findChild<QPushButton*>("keyClear")->isEnabled());
    dialog.findChild<QPushButton*>("keyClear")->click();
    CHECK(dialog.workingPreset().bindings.contains("plugin.listing.fine"));
    fineRow = rowFor(*tree, "plugin.listing.fine");
    REQUIRE(fineRow);
    CHECK(fineRow->text(1) == "—");  // Now an explicit "no key", not a suggestion.

    // Another preset gets no suggestion (they are keyed by preset name), and shows it unset.
    auto other = editor::hammerPreset();
    other.name = "Listing Keys";
    other.bindings["plugin.gone.act"] = editor::Binding{std::vector<editor::KeyChord>{editor::KeyChord::parse("F22").value()}};
    REQUIRE(window.presetStore().save(other));
    ui::SettingsDialog second(window);
    auto& presets = combo(second, "keyPresetCombo");
    presets.setCurrentIndex(presets.findData("Listing Keys"));
    emit presets.activated(presets.currentIndex());
    auto* tree2 = second.findChild<QTreeWidget*>("keyTree");
    REQUIRE(tree2);
    auto* otherFine = rowFor(*tree2, "plugin.listing.fine");
    REQUIRE(otherFine);
    CHECK(otherFine->text(1) == "—");
    // A binding for a plugin that is not loaded is listed, greyed and not selectable, and not removed.
    QTreeWidgetItem* dormant = nullptr;
    QTreeWidgetItemIterator it(tree2);
    for (; *it; ++it)
        if ((*it)->text(0) == "plugin.gone.act") dormant = *it;
    REQUIRE(dormant);
    CHECK(dormant->text(1) == "F22");
    CHECK(dormant->parent()->text(0).contains("not loaded"));
    CHECK(dormant->flags() == Qt::NoItemFlags);
    CHECK(second.workingPreset().bindings.contains("plugin.gone.act"));
    REQUIRE(window.presetStore().remove("Listing Keys"));
}

TEST_CASE("the Keybindings page lists the Blender preset's tool-scoped bindings by tool name") {
    ui_test::app();
    QSettings().setValue("editor/preset", "Blender");
    ui::MainWindow window;
    ui::SettingsDialog dialog(window);
    REQUIRE(dialog.workingPreset().name == "Blender");
    auto* tree = dialog.findChild<QTreeWidget*>("keyTree");
    REQUIRE(tree);
    auto* vertexMode = rowFor(*tree, "vertex.mode.vertex");
    auto* groups = rowFor(*tree, "selection.mode.groups");
    auto* applyClick = rowFor(*tree, "apply.click");
    REQUIRE(vertexMode);
    REQUIRE(groups);
    REQUIRE(applyClick);
    // The same key "1" is listed twice: once globally and once only while the Vertex tool is active.
    CHECK(vertexMode->text(1) == "1");
    CHECK(vertexMode->text(2) == "While Vertex tool is active");
    CHECK(groups->text(1) == "1");
    CHECK(groups->text(2).isEmpty());
    CHECK(applyClick->text(2) == "While Face edit tool is active");
    CHECK(editor::validate(dialog.workingPreset()).empty());
    QSettings().setValue("editor/preset", "Hammer");
}

TEST_CASE("plugin themes are registered read-only, shown in the choice and survive a custom rescan") {
    ui_test::app();
    QTemporaryDir plugin;
    REQUIRE(plugin.isValid());
    REQUIRE(QDir(plugin.path()).mkpath("themes"));
    const auto write = [&](const char* name, const char* text) {
        QFile f(plugin.path() + "/themes/" + QString::fromLatin1(name));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(text);
    };
    write("night.json", R"({"name":"Night Shift","base":"dark","colors":{"accent":"#ff8800"}})");
    write("broken.json", "{ not json");
    struct Cleanup {
        ~Cleanup() {
            ui::setPluginThemes({});
            QApplication* a = qobject_cast<QApplication*>(QCoreApplication::instance());
            if (a) ui::applyThemeId(*a, QString::fromLatin1(ui::kDefaultThemeId));
        }
    } cleanup;

    QStringList errors;
    ui::setPluginThemes({{"demo", "Demo Plugin", plugin.path() + "/themes"}}, &errors);
    const auto* theme = ui::findTheme("plugin:demo:night");
    REQUIRE(theme);
    CHECK(theme->name == "Night Shift");
    CHECK(theme->plugin == "Demo Plugin");
    CHECK_FALSE(theme->builtin);
    CHECK(ui::isPluginTheme(*theme));
    CHECK(theme->tokens.accent == QColor("#ff8800"));
    CHECK(theme->tokens.window == ui::findTheme("dark")->tokens.window);  // Everything else inherits from the base.
    CHECK(errors.size() == 1);
    CHECK(errors.front().contains("broken.json"));
    CHECK_FALSE(ui::findTheme("plugin:demo:broken"));

    // Rescanning the user's custom themes must not wipe the plugin's; plugin themes sort after custom ones.
    QTemporaryDir custom;
    REQUIRE(custom.isValid());
    ui::ThemeEntry mine = *ui::findTheme("dark");
    mine.name = "Mine";
    mine.builtin = false;
    REQUIRE_FALSE(ui::saveCustomTheme(mine, custom.path()).isEmpty());
    ui::loadCustomThemes(custom.path());
    REQUIRE(ui::findTheme("plugin:demo:night"));
    REQUIRE(ui::findTheme("custom:mine"));
    int customAt = -1, pluginAt = -1, i = 0;
    for (const auto& t : ui::themes()) {
        if (t.id == "custom:mine") customAt = i;
        if (t.id == "plugin:demo:night") pluginAt = i;
        ++i;
    }
    CHECK(customAt >= 0);
    CHECK(pluginAt > customAt);
    CHECK_FALSE(ui::removeCustomTheme("plugin:demo:night", custom.path()));  // Not a custom theme: nothing to delete.
    CHECK(ui::findTheme("plugin:demo:night"));

    // The theme choice labels it with its plugin, editing and deleting are off, and it can be applied.
    ui::MainWindow window;
    ui::SettingsDialog dialog(window);
    auto& choice = combo(dialog, "settingsTheme");
    const int row = choice.findData("plugin:demo:night");
    REQUIRE(row >= 0);
    CHECK(choice.itemText(row).contains("Demo Plugin"));
    choice.setCurrentIndex(row);
    CHECK_FALSE(dialog.findChild<QPushButton*>("themeEdit")->isEnabled());
    CHECK_FALSE(dialog.findChild<QPushButton*>("themeDelete")->isEnabled());
    REQUIRE(window.findChild<QAction*>("theme_plugin:demo:night"));
    window.setThemeId("plugin:demo:night");
    CHECK(ui::currentThemeId() == "plugin:demo:night");
    CHECK(ui::currentTokens().accent == QColor("#ff8800"));

    // Without the plugin (disabled, or --no-plugins) the theme is gone and the saved id falls back.
    ui::setPluginThemes({});
    CHECK_FALSE(ui::findTheme("plugin:demo:night"));
    ui::applyThemeId(*qobject_cast<QApplication*>(QCoreApplication::instance()), "plugin:demo:night");
    CHECK(ui::currentThemeId() == ui::kDefaultThemeId);
    CHECK(ui::findTheme("custom:mine"));
    ui::loadCustomThemes(QDir::tempPath() + "/apex_no_such_themes_dir");  // Leave the registry without the test's custom theme.
}
