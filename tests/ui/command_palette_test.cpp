#include <doctest/doctest.h>

#include <QAction>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QTest>

#include "ui/command_palette.hpp"
#include "ui/main_window.hpp"
#include "test_app.hpp"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif

using namespace apex;

namespace {
QStringList rows(const ui::CommandPalette& palette) {
    QStringList result;
    for (int i = 0; i < palette.list()->count(); ++i) result << palette.list()->item(i)->text();
    return result;
}
}  // namespace

TEST_CASE("fuzzy score matches subsequences and prefers tight word-start hits") {
    CHECK(ui::fuzzyScore("xyz", "Display mode") == std::nullopt);
    REQUIRE(ui::fuzzyScore("disp mode", "Display mode"));
    CHECK(ui::fuzzyScore("dm", "Display mode").value() > ui::fuzzyScore("dm", "Add some more").value());
    CHECK(ui::fuzzyScore("", "anything") == 0);
}

TEST_CASE("palette lists, filters, runs commands and drills into options") {
    ui_test::app();
    QWidget host;
    host.resize(800, 600);
    ui::CommandPalette palette(&host);
    int ran = 0;
    QString picked;
    palette.registry().addProvider("test", [&](ui::CommandRegistry& r) {
        r.add({.id = "t.run", .title = "Run thing", .category = "Test", .run = [&] { ++ran; }});
        r.add({.id = "t.off", .title = "Disabled thing", .category = "Test", .run = [&] { ran += 100; },
               .enabled = [] { return false; }});
        r.add({.id = "t.mode", .title = "Display mode", .category = "View",
               .options = [&] {
                   return std::vector<ui::PaletteOption>{{.label = "Wireframe", .run = [&] { picked = "wire"; }},
                                                         {.label = "Lit", .checked = true, .run = [&] { picked = "lit"; }}};
               }});
    });
    palette.open();
    CHECK(palette.isVisible());
    CHECK(palette.list()->count() == 3);

    QTest::keyClicks(palette.input(), "run");
    CHECK(rows(palette).first() == "Run thing");
    QTest::keyClick(palette.input(), Qt::Key_Return);
    CHECK(ran == 1);
    CHECK_FALSE(palette.isVisible());

    palette.open();
    QTest::keyClicks(palette.input(), "disabled");
    QTest::keyClick(palette.input(), Qt::Key_Return);  // Disabled: nothing runs, palette stays.
    CHECK(ran == 1);
    CHECK(palette.isVisible());

    palette.input()->clear();
    palette.input()->setText("display");
    CHECK(rows(palette).contains("Display mode: Lit"));  // Options are searchable directly.
    QTest::keyClick(palette.input(), Qt::Key_Tab);        // Tab completes the highlighted row.
    CHECK(palette.input()->text() == "Display mode: ");
    palette.input()->setText("display wire");
    QTest::keyClick(palette.input(), Qt::Key_Return);
    CHECK(picked == "wire");

    palette.open();
    palette.input()->setText("display mode");
    QTest::keyClick(palette.input(), Qt::Key_Return);  // The "Display mode…" row opens the options.
    CHECK(palette.isVisible());
    CHECK(rows(palette) == QStringList({"Wireframe", "Lit"}));
    QTest::keyClick(palette.input(), Qt::Key_Escape);  // Back to the command list.
    CHECK(palette.isVisible());
    CHECK(palette.list()->count() == 3);
    QTest::keyClick(palette.input(), Qt::Key_Escape);
    CHECK_FALSE(palette.isVisible());
}

TEST_CASE("main window exposes the command palette action with a default binding") {
    ui_test::app();
    ui::MainWindow window;
    auto* action = window.findChild<QAction*>("app.command_palette");
    REQUIRE(action);
    CHECK(action->shortcut().toString() == "Ctrl+Shift+P");
}
