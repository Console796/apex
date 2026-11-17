#include "test_app.hpp"
#include "ui/plugin_form.hpp"
#include <doctest/doctest.h>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTest>

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
    apex_ui_form form{sizeof(form), controls, 5};
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
    apex_ui_form form{sizeof(form), controls, 2};
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
    apex_ui_form form{sizeof(form), &section, 1};
    REQUIRE(widget.describe(form));
    REQUIRE(widget.findChild<QLineEdit*>("child"));
    CHECK(widget.findChild<QLineEdit*>("child")->text() == "value");
}
