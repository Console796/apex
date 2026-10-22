// Headless shell tests; run with QT_QPA_PLATFORM=offscreen (set by CTest).
#include <doctest/doctest.h>

#include <QApplication>
#include <QAction>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include "core/log.hpp"
#include "../editor/fixtures.hpp"
#include "document/document.hpp"
#include "ui/log_panel.hpp"
#include "ui/key_chord.hpp"
#include "ui/main_window.hpp"
#include "ui/outliner_panel.hpp"
#include "ui/theme.hpp"
#include "ui/viewport_area.hpp"
#include "ui/viewport_pane.hpp"
#include <QMenu>
#include <QSplitter>

namespace {
QApplication& app() {
    static int argc = 1;
    static char name[] = "apex_ui_tests";
    static char* argv[] = {name, nullptr};
    static QTemporaryDir config;  // Keep QSettings out of the user's real config.
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("ApexTest"));
    QCoreApplication::setApplicationName(QStringLiteral("ApexTest"));
    static QApplication a(argc, argv);
    return a;
}

const char* kVmf = R"(versioninfo { "editorversion" "400" }
visgroups { visgroup { "name" "Alpha" "visgroupid" "1" } }
world { "id" "1" "classname" "worldspawn"
  solid { "id" "2" }
  group { "id" "3" }
}
entity { "id" "4" "classname" "info_target" "targetname" "t" }
)";
}  // namespace

TEST_CASE("theme tokens expand into QSS with no placeholders left") {
    app();
    for (auto theme : {apex::ui::Theme::Dark, apex::ui::Theme::Light}) {
        const auto qss = apex::ui::renderQss(apex::ui::tokens(theme));
        CHECK_FALSE(qss.contains(QLatin1Char('@')));
        CHECK(qss.contains(apex::ui::tokens(theme).window.name()));
    }
    CHECK(apex::ui::tokens(apex::ui::Theme::Dark).window != apex::ui::tokens(apex::ui::Theme::Light).window);
}

TEST_CASE("shell opens a VMF, fills the outliner and round-trips on save") {
    auto& a = app();
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
    QTemporaryDir dir;
    const QString in = dir.filePath("t.vmf");
    {
        QFile f(in);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(kVmf);
    }
    apex::ui::MainWindow w;
    w.show();
    CHECK(w.viewportArea() != nullptr);
    CHECK(w.document() == nullptr);

    apex::assets::GameProfile p;
    p.name = "Test Game";
    w.setProfiles({p});
    CHECK(w.profilePicker()->count() == 1);
    REQUIRE(w.currentProfile() != nullptr);
    CHECK(w.currentProfile()->name == "Test Game");

    REQUIRE(w.openFile(in));
    CHECK(w.document() != nullptr);
    auto* tree = w.outliner();
    REQUIRE(tree->topLevelItemCount() == 4);  // World, Entities, Groups, Visgroups
    CHECK(tree->topLevelItem(0)->text(0).startsWith("World"));
    CHECK(tree->topLevelItem(1)->childCount() == 1);
    CHECK(tree->topLevelItem(2)->childCount() == 1);
    CHECK(tree->topLevelItem(3)->childCount() == 1);
    CHECK(w.recentFiles().contains(QFileInfo(in).absoluteFilePath()));

    const QString out = dir.filePath("o.vmf");
    CHECK(w.saveFile(out));
    CHECK(QFileInfo(out).size() > 0);

    w.setTheme(apex::ui::Theme::Light);
    CHECK(apex::ui::currentTheme() == apex::ui::Theme::Light);
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
}

TEST_CASE("log panel receives core/log lines") {
    app();
    apex::ui::LogPanel panel;
    apex::log::warn("hello {}", 42);
    QCoreApplication::processEvents();
    CHECK(panel.toPlainText().contains("hello 42"));
}

TEST_CASE("viewport area mirrors the layout tree into splitters and persists it") {
    auto& a = app();
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
    using namespace apex;
    ui::ViewportArea area;
    area.resize(800, 600);
    area.show();
    CHECK(area.paneCount() == 4);
    CHECK(area.findChildren<ui::PaneFrame*>().size() == 4);
    CHECK(area.findChildren<QSplitter*>().size() == 3);  // Quad: one root split plus two.

    // Split a pane: the rebuild is deferred to the next event-loop turn, then the tree is exact.
    const auto first = area.viewportLayout().paneIds().front();
    emit area.frame(first)->splitRequested(first, viewport::Orientation::Horizontal);
    CHECK(area.paneCount() == 5);
    area.flush();
    CHECK(area.findChildren<ui::PaneFrame*>().size() == 5);

    // Maximize shows exactly one pane without touching the tree.
    area.mutableLayout().toggleMaximize(first);
    area.setLayout(viewport::ViewportLayout(area.viewportLayout()));
    CHECK(area.findChildren<ui::PaneFrame*>().size() == 1);
    area.mutableLayout().restore();

    // Layout JSON round trip.
    const auto json = area.layoutJson();
    ui::ViewportArea other;
    REQUIRE(other.restoreLayoutJson(json));
    CHECK(other.paneCount() == 5);
    CHECK_FALSE(other.restoreLayoutJson("not json"));
    CHECK(other.paneCount() == 5);

    area.resetLayout();
    CHECK(area.paneCount() == 4);
}

TEST_CASE("pane menu offers the full set of view controls") {
    auto& a = app();
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
    apex::ui::ViewportArea area;
    area.show();
    auto* frame = area.findChildren<apex::ui::PaneFrame*>().front();
    emit frame->menu()->aboutToShow();
    QStringList titles;
    for (auto* act : frame->menu()->actions()) titles << act->text();
    for (const char* want : {"Preset", "Projection", "Axis", "Display mode", "Overlays", "Maximize", "Close pane"})
        CHECK(titles.contains(QString::fromLatin1(want)));
    CHECK(titles.filter("Split").size() == 2);
}

TEST_CASE("map views load into and save from the open viewports") {
    auto& a = app();
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
    QTemporaryDir dir;
    QFile src(QStringLiteral(APEX_FIXTURE_DIR "/probe/e_control.vmf"));
    REQUIRE(src.open(QIODevice::ReadOnly));
    const QByteArray original = src.readAll();
    const QString in = dir.filePath("v.vmf");
    {
        QFile f(in);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(original);
    }
    apex::ui::MainWindow w;
    w.show();
    REQUIRE(w.openFile(in));
    const auto* cam = w.viewportArea()->viewportLayout().viewport(w.viewportArea()->viewportLayout().paneIds()[0]);
    REQUIRE(cam != nullptr);
    CHECK(cam->camera.position == apex::DVec3{0, -300, 100});

    // An unchanged save keeps the views byte-identical.
    const QString out = dir.filePath("o.vmf");
    REQUIRE(w.saveFile(out));
    QFile f(out);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const auto saved = f.readAll();
    CHECK(saved.contains("\"position\" \"(0 -300 100)\""));
}

TEST_CASE("selection menu preset switch and outliner share the editor session") {
    app();
    QSettings().setValue("editor/preset", "Hammer");
    QSettings().setValue("editor/marqueeTouching", false);
    QTemporaryDir dir;
    const auto path = dir.filePath("selection.vmf");
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(kVmf);
    }
    apex::ui::MainWindow window;
    window.show();
    REQUIRE(window.openFile(path));
    QCoreApplication::processEvents();
    auto& session = window.editorSession();
    const auto action = [&](const char* id) {
        auto* a = window.findChild<QAction*>(QString::fromLatin1(id));
        REQUIRE(a != nullptr);
        return a;
    };
    const auto perform = [&](const char* id) { action(id)->trigger(); };
    perform("selection.mode.objects");
    CHECK(session.selection().mode() == apex::editor::SelectionMode::Objects);
    perform("selection.all");
    CHECK(session.selection().ordered().size() == 2);
    CHECK(window.document()->undoCount() == 0);
    perform("selection.none");
    CHECK(session.selection().ordered().empty());
    perform("selection.invert");
    CHECK(session.selection().ordered().size() == 2);
    perform("selection.marquee_toggle");
    CHECK(session.marqueeMode() == apex::editor::MarqueeMode::Touching);
    CHECK(action("selection.marquee_toggle")->isChecked());
    CHECK(QSettings().value("editor/marqueeTouching").toBool());
    perform("selection.ignore_groups");
    CHECK(session.selection().ignoreGroups());
    const auto hammer = action("selection.all")->shortcut();
    CHECK(hammer == QKeySequence("Ctrl+A"));
    perform("preset.Blender");
    CHECK(action("selection.all")->shortcut() == QKeySequence("A"));
    CHECK(action("selection.all")->shortcut() != hammer);
    CHECK(session.preset().name == "Blender");
    CHECK(QSettings().value("editor/preset").toString() == "Blender");
    {
        apex::ui::MainWindow reopened;
        CHECK(reopened.editorSession().preset().name == "Blender");
        CHECK(reopened.editorSession().marqueeMode() == apex::editor::MarqueeMode::Touching);
    }

    perform("selection.none");
    auto* tree = window.outliner();
    auto* row = tree->topLevelItem(1)->child(0);
    tree->scrollToItem(row);
    QCoreApplication::processEvents();
    const auto point = tree->visualItemRect(row).center();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(tree->viewport()->mapToGlobal(point)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(tree->viewport()->mapToGlobal(point)),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(tree->viewport(), &press);
    QCoreApplication::sendEvent(tree->viewport(), &release);
    const apex::document::NodeId node = row->data(0, Qt::UserRole).toULongLong();
    CHECK(session.selection().ordered() == std::vector<apex::editor::Target>{node});
    CHECK(row->isSelected());
    session.edit("Delete entity", [&](apex::document::Transaction& tx) {
        tx.destroy(*window.document()->handle(node));
    });
    CHECK(session.selection().ordered().empty());
    REQUIRE(action("edit.undo")->isEnabled());
    perform("edit.undo");
    CHECK(session.selection().ordered() == std::vector<apex::editor::Target>{node});
    REQUIRE(tree->selectedItems().size() == 1);
    CHECK(tree->selectedItems().front()->data(0, Qt::UserRole).toULongLong() == node);
    perform("edit.redo");
    CHECK(session.selection().ordered().empty());
    CHECK(tree->selectedItems().empty());
    perform("selection.mode.faces");
    CHECK(action("selection.mode.faces")->isChecked());
    CHECK_FALSE(action("selection.mode.objects")->isChecked());

    // Reopening detaches every observer before the old document is destroyed.
    REQUIRE(window.openFile(path));
    CHECK(session.selection().ordered().empty());
    perform("preset.Hammer");
}

TEST_CASE("UI key chord conversion round trips preset shortcuts") {
    app();
    for (const auto* preset : {&apex::editor::hammerPreset(), &apex::editor::blenderPreset()}) {
        for (const auto& [id, binding] : preset->bindings) {
            const auto* chords = std::get_if<std::vector<apex::editor::KeyChord>>(&binding.value);
            if (!chords) continue;
            for (const auto& chord : *chords) {
                if (chord.key == "Ctrl" || chord.key == "Alt") continue;
                const auto sequence = apex::ui::keySequence(chord);
                REQUIRE_FALSE(sequence.isEmpty());
                const auto combination = sequence[0];
                QKeyEvent event(QEvent::KeyPress, combination.key(), combination.keyboardModifiers());
                CHECK(apex::ui::keyChord(event) == chord);
            }
        }
    }
}

TEST_CASE("grid shortcuts and work plane session changes update shell status") {
    app();
    QSettings().setValue("editor/preset", "Hammer");
    apex::ui::MainWindow window;
    window.show();
    REQUIRE(window.openFile(QStringLiteral(APEX_FIXTURE_DIR "/probe/e_control.vmf")));
    window.activateWindow();
    window.setFocus();
    QCoreApplication::processEvents();
    auto* grid = window.findChild<QLabel*>("statusGrid");
    auto* plane = window.findChild<QLabel*>("statusPlane");
    REQUIRE(grid);
    REQUIRE(plane);
    CHECK(grid->text() == "Grid: 64");
    CHECK(plane->text() == "Plane: ground");
    const auto press = [&](int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers);
        QCoreApplication::sendEvent(&window, &event);
        QCoreApplication::processEvents();
    };
    press(Qt::Key_BracketRight);
    CHECK(grid->text() == "Grid: 128");
    press(Qt::Key_BracketLeft);
    CHECK(grid->text() == "Grid: 64");
    auto& session = window.editorSession();
    session.setGridSize(0.125);
    CHECK(grid->text() == "Grid: 0.125");
    // Drive CPU hover directly: the offscreen platform has no Vulkan pane.
    editor_test::addBox(*window.document(), {200000, 199990, 199990}, {200010, 200010, 200010});
    session.pointerHover({{199900, 200000, 200000}, {1, 0, 0}});
    CHECK(plane->text() == "Plane: face (hover)");
    press(Qt::Key_P, Qt::AltModifier);
    CHECK(plane->text() == "Plane: pinned");
    session.clearHover();
    CHECK(plane->text() == "Plane: pinned");
    press(Qt::Key_Escape);
    CHECK(plane->text() == "Plane: ground");
    session.pointerHover({{199900, 200000, 200000}, {1, 0, 0}});
    session.pinWorkPlane();
    session.unpinWorkPlane();
    CHECK(plane->text() == "Plane: face (hover)");
    session.clearHover();
    CHECK(plane->text() == "Plane: ground");
    auto* menu = window.findChild<QMenu*>("menuGrid");
    REQUIRE(menu);
    for (const auto* id : {"grid.larger", "grid.smaller", "workplane.pin",
                           "workplane.axis_x", "workplane.axis_y", "workplane.axis_z"}) {
        auto* action = window.findChild<QAction*>(id);
        REQUIRE(action);
        CHECK(menu->actions().contains(action));
    }
}

TEST_CASE("pane grids use surface bounds pinned precedence and every axis fallback") {
    using namespace editor_test;
    Document doc(parsed());
    addBox(doc, {0, 0, 0}, {10, 20, 30});
    EditorSession session;
    session.attach(doc);
    session.setGridSize(0.5);
    auto perspective = viewport::makePreset(viewport::Preset::Camera);
    auto grid = ui::makeGridSettings(perspective, &session);
    CHECK(grid.enabled);
    CHECK(grid.spacing == 0.5f);
    CHECK(grid.origin == DVec3{0});
    CHECK(grid.normal == DVec3{0, 0, 1});
    CHECK(grid.uAxis == DVec3{1, 0, 0});
    CHECK(grid.vAxis == DVec3{0, 1, 0});
    CHECK_FALSE(grid.bounds);
    session.pointerHover({{-100, 5, 5}, {1, 0, 0}});
    grid = ui::makeGridSettings(perspective, &session);
    CHECK(grid.normal == DVec3{-1, 0, 0});
    CHECK(grid.origin == DVec3{0, 10, 15});
    const auto plane = session.resolvedWorkPlane()->plane;
    REQUIRE(grid.bounds);
    CHECK(grid.bounds->uMin == plane.bounds->min.x);
    CHECK(grid.bounds->uMax == plane.bounds->max.x);
    CHECK(grid.bounds->vMin == plane.bounds->min.y);
    CHECK(grid.bounds->vMax == plane.bounds->max.y);
    for (const auto axis : {viewport::ViewAxis::PosX, viewport::ViewAxis::NegX,
                            viewport::ViewAxis::PosY, viewport::ViewAxis::NegY,
                            viewport::ViewAxis::PosZ, viewport::ViewAxis::NegZ}) {
        CAPTURE(axis);
        auto view = viewport::makePreset(viewport::Preset::Top);
        view.camera.axis = axis;
        const auto basis = view.camera.basis();
        grid = ui::makeGridSettings(view, &session);
        CHECK(grid.enabled);
        CHECK_FALSE(grid.bounds);
        checkPoint(grid.normal, -basis.forward);
        checkPoint(grid.uAxis, basis.right);
        checkPoint(grid.vAxis, basis.up);
        session.pinWorkPlane();
        grid = ui::makeGridSettings(view, &session);
        checkPoint(grid.normal, plane.normal);
        checkPoint(grid.origin, plane.origin);
        REQUIRE(grid.bounds);
        view.overlays.grid = false;
        CHECK_FALSE(ui::makeGridSettings(view, &session).enabled);
        session.unpinWorkPlane();
    }
    session.clearHover();
    auto freeOrtho = viewport::makePreset(viewport::Preset::Camera);
    freeOrtho.camera.projection = viewport::ProjectionKind::Orthographic;
    grid = ui::makeGridSettings(freeOrtho, &session);
    CHECK(grid.origin == DVec3{0});
    CHECK(grid.normal == DVec3{0, 0, 1});
    CHECK_FALSE(grid.bounds);
}
