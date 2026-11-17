#include "test_app.hpp"
#include "ui/plugin_form.hpp"
#include <doctest/doctest.h>
#include <QLineEdit>
#include <QApplication>
#include <QCheckBox>
#include <iterator>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTest>
#include <QColorDialog>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QTimer>

namespace {
apex_str str(const char* s) { return {s, std::char_traits<char>::length(s)}; }
apex_ui_control field(const char* key, const char* value) {
    apex_ui_control c{}; c.struct_size = sizeof(c); c.kind = APEX_UI_TEXT;
    c.key = str(key); c.label = str(key); c.value.struct_size = sizeof(c.value); c.value.text = str(value);
    return c;
}

TEST_CASE("plugin form sends changed values and button clicks") {
    (void)apex::ui_test::app();
    apex::ui::PluginForm widget;
    apex_ui_control controls[5]{};
    for (auto& control : controls) { control.struct_size = sizeof(control); control.value.struct_size = sizeof(control.value); }
    controls[0] = field("text", "before");
    controls[1].kind = APEX_UI_NUMBER; controls[1].key = str("number"); controls[1].minimum = 0; controls[1].maximum = 100;
    controls[2].kind = APEX_UI_CHECKBOX; controls[2].key = str("check");
    apex_str options[] = {str("one"), str("two")};
    controls[3].kind = APEX_UI_CHOICE; controls[3].key = str("choice"); controls[3].options = options; controls[3].option_count = 2;
    controls[3].value.text = str("one");
    controls[4].kind = APEX_UI_BUTTON; controls[4].key = str("button");
    apex_ui_form form{sizeof(form), 0, controls, 5};
    REQUIRE(widget.describe(form));
    std::string key, value; double number = 0; bool checked = false; bool clicked = false;
    widget.setChange([&](std::string_view k, const apex_ui_value& v) {
        key = k; value = v.text.data ? std::string(v.text.data, v.text.size) : std::string{};
        number = v.number; checked = v.checked != 0;
    });
    widget.setClick([&](std::string_view k) { clicked = k == "button"; });
    auto* edit = widget.findChild<QLineEdit*>("text"); REQUIRE(edit);
    edit->clear(); QTest::keyClicks(edit, "new"); CHECK(key == "text"); CHECK(value == "new");
    widget.findChild<QDoubleSpinBox*>("number")->setValue(12); CHECK(key == "number"); CHECK(number == 12);
    widget.findChild<QCheckBox*>("check")->setChecked(true); CHECK(key == "check"); CHECK(checked);
    widget.findChild<QComboBox*>("choice")->setCurrentIndex(1); CHECK(key == "choice"); CHECK(value == "two");
    widget.findChild<QPushButton*>("button")->click(); CHECK(clicked);
}
}
TEST_CASE("plugin form retains focus when another value changes") {
    (void)apex::ui_test::app();
    apex::ui::PluginForm widget;
    apex_ui_control controls[] = {field("first", "one"), field("second", "two")};
    apex_ui_form form{sizeof(form), 0, controls, 2};
    REQUIRE(widget.describe(form));
    widget.show();
    auto* first = widget.findChild<QLineEdit*>("first");
    REQUIRE(first);
    first->setFocus();
    QCoreApplication::processEvents();
    REQUIRE(first->hasFocus());
    controls[1].value.text = str("changed");
    REQUIRE(widget.describe(form));
    CHECK(widget.findChild<QLineEdit*>("first") == first);
    CHECK(first->hasFocus());
    CHECK(widget.findChild<QLineEdit*>("second")->text() == "changed");
}

TEST_CASE("plugin form describes a collapsible section") {
    (void)apex::ui_test::app();
    apex::ui::PluginForm widget;
    auto child = field("child", "value");
    apex_ui_control section{}; section.struct_size = sizeof(section); section.value.struct_size = sizeof(section.value);
    section.kind = APEX_UI_SECTION; section.key = str("section"); section.label = str("Section");
    section.children = &child; section.child_count = 1;
    apex_ui_form form{sizeof(form), 0, &section, 1};
    REQUIRE(widget.describe(form));
    REQUIRE(widget.findChild<QLineEdit*>("child"));
    CHECK(widget.findChild<QLineEdit*>("child")->text() == "value");
}

TEST_CASE("every plugin form control kind round trips its value") {
    (void)apex::ui_test::app();
    apex::ui::PluginForm widget;
    const auto make = [](uint32_t kind, const char* key) {
        apex_ui_control c{}; c.struct_size = sizeof(c); c.kind = kind; c.key = str(key); c.label = str(key);
        c.value.struct_size = sizeof(c.value); return c;
    };
    apex_str choices[] = {str("one"), str("two"), str("three")};
    apex_str rows[] = {str("alpha"), str("beta"), str("gamma")};
    apex_str materials[] = {str("dev/a"), str("dev/b")};
    auto label = make(APEX_UI_LABEL, "label"); label.value.text = str("hello");
    auto text = make(APEX_UI_TEXT, "text"); text.value.text = str("abc");
    auto number = make(APEX_UI_NUMBER, "number"); number.minimum = -10; number.maximum = 10; number.value.number = 2.5;
    auto check = make(APEX_UI_CHECKBOX, "check"); check.value.checked = 1;
    auto choice = make(APEX_UI_CHOICE, "choice"); choice.options = choices; choice.option_count = 3; choice.value.text = str("two");
    auto colour = make(APEX_UI_COLOUR, "colour"); colour.value.text = str("#ff102030");
    auto table = make(APEX_UI_TABLE, "table"); table.options = rows; table.option_count = 3; table.value.text = str("beta");
    auto material = make(APEX_UI_MATERIAL, "material"); material.options = materials; material.option_count = 2; material.value.text = str("dev/b");
    auto model = make(APEX_UI_MODEL, "model"); model.value.text = str("props/a.mdl");
    auto sound = make(APEX_UI_SOUND, "sound"); sound.value.text = str("ambient/a.wav");
    auto child = make(APEX_UI_TEXT, "nested"); child.value.text = str("inner");
    auto section = make(APEX_UI_SECTION, "section"); section.children = &child; section.child_count = 1;
    apex_ui_control controls[] = {label, text, number, check, choice, colour, table, material, model, sound, section};
    apex_ui_form form{sizeof(form), 0, controls, std::size(controls)};
    REQUIRE(widget.describe(form));
    widget.show();
    // Plugin -> widget.
    CHECK(widget.findChild<QLabel*>("label")->text() == "hello");
    CHECK(widget.findChild<QLineEdit*>("text")->text() == "abc");
    CHECK(widget.findChild<QDoubleSpinBox*>("number")->value() == 2.5);
    CHECK(widget.findChild<QCheckBox*>("check")->isChecked());
    CHECK(widget.findChild<QComboBox*>("choice")->currentText() == "two");
    CHECK(widget.findChild<QPushButton*>("colour")->text() == "#ff102030");
    CHECK(widget.findChild<QListWidget*>("table")->currentItem()->text() == "beta");
    CHECK(widget.findChild<QComboBox*>("material")->currentText() == "dev/b");
    CHECK(widget.findChild<QComboBox*>("model")->currentText() == "props/a.mdl");
    CHECK(widget.findChild<QComboBox*>("sound")->currentText() == "ambient/a.wav");
    CHECK(widget.findChild<QLineEdit*>("nested")->text() == "inner");
    // Widget -> plugin: the value the callback sees is the one the widget shows.
    std::string key, value; double numberValue = 0; bool checked = false;
    widget.setChange([&](std::string_view k, const apex_ui_value& v) {
        key = k; value = v.text.data ? std::string(v.text.data, v.text.size) : std::string{};
        numberValue = v.number; checked = v.checked != 0;
    });
    widget.findChild<QDoubleSpinBox*>("number")->setValue(-3.5);
    CHECK(key == "number"); CHECK(numberValue == -3.5);
    widget.findChild<QCheckBox*>("check")->setChecked(false);
    CHECK(key == "check"); CHECK_FALSE(checked);
    widget.findChild<QComboBox*>("choice")->setCurrentIndex(2);
    CHECK(key == "choice"); CHECK(value == "three");
    widget.findChild<QListWidget*>("table")->setCurrentRow(2);
    CHECK(key == "table"); CHECK(value == "gamma");
    for (const auto* picker : {"material", "model", "sound"}) {
        auto* combo = widget.findChild<QComboBox*>(picker);
        combo->setEditText("typed/" + QString(picker));
        CHECK(key == picker); CHECK(value == "typed/" + std::string(picker));
    }
    auto* nested = widget.findChild<QLineEdit*>("nested");
    nested->clear(); QTest::keyClicks(nested, "deep");
    CHECK(key == "nested"); CHECK(value == "deep");
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QColorDialog*>(QApplication::activeModalWidget());
        if (dialog) { dialog->setCurrentColor(QColor(0x11, 0x22, 0x33)); dialog->accept(); }
    });
    widget.findChild<QPushButton*>("colour")->click();
    CHECK(key == "colour"); CHECK(value == "#ff112233");
    CHECK(widget.findChild<QPushButton*>("colour")->text() == "#ff112233");
    // Re-describing with the plugin's own new values shows them without echoing a change.
    key.clear();
    controls[2].value.number = 7; controls[4].value.text = str("one"); controls[6].value.text = str("alpha");
    REQUIRE(widget.describe(form));
    CHECK(widget.findChild<QDoubleSpinBox*>("number")->value() == 7);
    CHECK(widget.findChild<QComboBox*>("choice")->currentText() == "one");
    CHECK(widget.findChild<QListWidget*>("table")->currentItem()->text() == "alpha");
    CHECK(key.empty());
}
