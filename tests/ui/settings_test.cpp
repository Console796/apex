// Headless tests for the Preferences dialog and keybinding editor.
#include "test_app.hpp"
#include <doctest/doctest.h>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>

#include "editor/preset_io.hpp"
#include "ui/main_window.hpp"
#include "ui/settings_dialog.hpp"

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

TEST_CASE("new solid preferences and status toggle persist material and UV alignment") {
    ui_test::app();
    QSettings settings;
    const auto oldMaterial = settings.value("editor/newSolidMaterial");
    const auto oldWorld = settings.value("editor/newSolidWorldUv");
    settings.setValue("editor/newSolidMaterial", "test/loaded");
    settings.setValue("editor/newSolidWorldUv", true);
    {
        ui::MainWindow window;
        CHECK(window.editorSession().newSolidStyle().material == "test/loaded");
        CHECK(window.editorSession().newSolidStyle().alignment == geometry::UvAlignment::World);
        auto* toggle = window.findChild<QAction*>("newSolidWorldUv");
        REQUIRE(toggle);
        CHECK(toggle->isChecked());
        toggle->trigger();
        CHECK(window.editorSession().newSolidStyle().alignment == geometry::UvAlignment::Face);
        CHECK_FALSE(settings.value("editor/newSolidWorldUv").toBool());
        ui::SettingsDialog dialog(window);
        auto* material = dialog.findChild<QLineEdit*>("settingsNewSolidMaterial");
        auto* alignment = dialog.findChild<QComboBox*>("settingsUvAlignment");
        REQUIRE(material);
        REQUIRE(alignment);
        material->setText("test/changed");
        alignment->setCurrentIndex(1);
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        REQUIRE(buttons);
        buttons->button(QDialogButtonBox::Apply)->click();
        CHECK(window.editorSession().newSolidStyle().material == "test/changed");
        CHECK(toggle->isChecked());
    }
    {
        ui::MainWindow window;
        CHECK(window.editorSession().newSolidStyle().material == "test/changed");
        CHECK(window.editorSession().newSolidStyle().alignment == geometry::UvAlignment::World);
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
}
