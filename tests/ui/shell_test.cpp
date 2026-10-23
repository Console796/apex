// Headless shell tests; run with QT_QPA_PLATFORM=offscreen (set by CTest).
#include "test_app.hpp"
#include <doctest/doctest.h>

#include <QApplication>
#include <QAbstractItemDelegate>
#include <QTreeWidgetItemIterator>
#include <QMimeData>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QAction>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTabBar>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/log.hpp"
#include "../editor/fixtures.hpp"
#include "document/document.hpp"
#include "ui/log_panel.hpp"
#include "ui/key_chord.hpp"
#include "ui/main_window.hpp"
#include "ui/outliner_panel.hpp"
#include "ui/transform_dialog.hpp"
#include "ui/transform_input.hpp"
#include "viewport/pick_math.hpp"
#include <QLineEdit>
#include <QCheckBox>
#include <QTabWidget>
#include <QToolButton>
#include <QToolBar>
#include "ui/theme.hpp"
#include "ui/viewport_area.hpp"
#include "ui/viewport_pane.hpp"
#include <QMenu>
#include <QSplitter>

namespace {
QApplication& app() { return apex::ui_test::app(); }

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

TEST_CASE("built-in themes are distinct and fully expand") {
    app();
    const auto& all = apex::ui::themes();
    REQUIRE(all.size() >= 6);
    for (const auto& t : all) {
        CHECK_FALSE(apex::ui::renderQss(t.tokens).contains(QLatin1Char('@')));
        for (const auto& u : all)
            if (&t != &u) CHECK(t.id != u.id);
    }
    CHECK(apex::ui::findTheme("solarized-light") != nullptr);
}

TEST_CASE("custom themes round-trip through files and inherit from a base") {
    auto& a = app();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto custom = *apex::ui::findTheme("midnight");
    custom.name = "My Theme!";
    custom.tokens.accent = QColor("#12ab34");
    custom.tokens.radius = 9;
    QString error;
    const QString id = apex::ui::saveCustomTheme(custom, dir.path(), &error);
    CHECK(id == "custom:my-theme");
    const auto* loaded = apex::ui::findTheme(id);
    REQUIRE(loaded);
    CHECK_FALSE(loaded->builtin);
    CHECK(loaded->name == "My Theme!");
    CHECK(loaded->tokens.accent == QColor("#12ab34"));
    CHECK(loaded->tokens.radius == 9);
    CHECK(loaded->tokens.window == custom.tokens.window);

    // A sparse file inherits everything else from its base; a bad colour is rejected.
    apex::ui::ThemeEntry sparse;
    CHECK(apex::ui::themeFromJson(QJsonDocument::fromJson(R"({"name":"S","base":"light","colors":{"accent":"#ff0000"}})").object(), sparse));
    CHECK(sparse.tokens.accent == QColor("#ff0000"));
    CHECK(sparse.tokens.window == apex::ui::tokens(apex::ui::Theme::Light).window);
    CHECK_FALSE(apex::ui::themeFromJson(QJsonDocument::fromJson(R"({"name":"B","colors":{"accent":"nope"}})").object(), sparse, &error));

    CHECK(apex::ui::applyThemeId(a, id) == id);
    CHECK(apex::ui::currentThemeId() == id);
    CHECK(apex::ui::removeCustomTheme(id, dir.path()));
    CHECK(apex::ui::findTheme(id) == nullptr);
    CHECK(apex::ui::applyThemeId(a, "gone") == "dark");
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

TEST_CASE("input trace marker is available on F9") {
    app();
    apex::ui::MainWindow window;
    auto* marker = window.findChild<QAction*>(QStringLiteral("actionInputTraceMarker"));
    REQUIRE(marker);
    CHECK(marker->shortcut() == QKeySequence(Qt::Key_F9));
    CHECK(marker->shortcutContext() == Qt::ApplicationShortcut);
    marker->trigger();
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
    for (const char* want : {"Preset", "Projection", "Axis", "Display mode", "Overlays", "Maximize", "Close area"})
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
    const auto* cam = w.viewportArea()->viewportLayout().viewport(w.viewportArea()->viewportLayout().viewportIds()[0]);
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

TEST_CASE("pane world axes run from the origin in axis colours and follow the grid overlay") {
    using namespace editor_test;
    auto view = viewport::makePreset(viewport::Preset::Camera);
    render::DrawList list;
    ui::appendWorldAxes(list, view);
    REQUIRE(list.worldSegments.size() == 3);
    const std::array<DVec3, 3> directions{DVec3{1, 0, 0}, DVec3{0, 1, 0}, DVec3{0, 0, 1}};
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(list.worldSegments[i].a == DVec3{0});
        CHECK(glm::normalize(list.worldSegments[i].b) == directions[i]);
        CHECK(list.worldSegments[i].colour[static_cast<int>(i)] ==
              std::max({list.worldSegments[i].colour.r, list.worldSegments[i].colour.g, list.worldSegments[i].colour.b}));
    }
    view.overlays.grid = false;
    list = {};
    ui::appendWorldAxes(list, view);
    CHECK(list.worldSegments.empty());
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
    CHECK(grid.origin == DVec3{0, 0, 0}); // World-anchored (0.2 D9).
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

namespace {
using namespace apex;

struct TransformScene {
    ui::MainWindow window;
    document::NodeHandle box;
    document::NodeId boxId = 0;

    explicit TransformScene(const char* preset = "Hammer") {
        app();
        QSettings().setValue("editor/preset", preset);
        window.show();
        REQUIRE(window.openFile(QStringLiteral(APEX_FIXTURE_DIR "/probe/e_control.vmf")));
        box = editor_test::addBox(*window.document(), {5, 5, 5}, {69, 69, 69});
        boxId = editor_test::id(*window.document(), box);
        auto& session = window.editorSession();
        session.perform("selection.mode.solids");
        session.selectNode(boxId);
        QCoreApplication::processEvents();
    }
    editor::EditorSession& session() { return window.editorSession(); }
    QAction* action(const char* id) {
        auto* a = window.findChild<QAction*>(id);
        REQUIRE(a);
        return a;
    }
    std::size_t undo() { return window.document()->undoCount(); }
    DVec3 min() {
        const auto solid = geometry::solidGeometry(*window.document()->get(box)->block);
        REQUIRE(solid);
        DVec3 m{1e9};
        for (const auto& v : solid->poly.vertices) m = glm::min(m, v);
        return m;
    }
};

viewport::Camera cameraLooking(DVec3 from, DVec3 at) {
    viewport::Camera camera;
    camera.position = from;
    const DVec3 f = glm::normalize(at - from);
    camera.angles = {glm::degrees(-std::asin(f.z)), glm::degrees(std::atan2(f.y, f.x)), 0.0};
    return camera;
}

const QSizeF kPane{800, 600};

QMouseEvent mouseEvent(QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons,
                       Qt::KeyboardModifiers mods = Qt::NoModifier) {
    return QMouseEvent(type, at, at, button, buttons, mods);
}

std::optional<QPointF> findHandle(editor::EditorSession& session, const viewport::Camera& camera,
                                  editor::GizmoHandle handle) {
    const auto context = session.gizmoContext();
    if (!context) return std::nullopt;
    const auto view = ui::gizmoViewFor(camera, kPane, context->pivot);
    for (double y = 0; y < kPane.height(); y += 2) {
        for (double x = 0; x < kPane.width(); x += 2) {
            const auto ray = viewport::pixelRay(camera, kPane.width() / kPane.height(), x, y, kPane.width(), kPane.height());
            const auto hit = editor::hitTestGizmo(session.gizmoMode(), *context, view, ray);
            if (hit && hit->handle == handle) return QPointF(x, y);
        }
    }
    return std::nullopt;
}

Ray rayAtPixel(const viewport::Camera& camera, QPointF at) {
    return viewport::pixelRay(camera, kPane.width() / kPane.height(), at.x(), at.y(), kPane.width(), kPane.height());
}
}  // namespace

TEST_CASE("mode summary tracks tool, selection mode and gizmo") {
    TransformScene scene;
    auto& session = scene.session();
    const auto* label = scene.window.findChild<QLabel*>("statusMode");
    REQUIRE(label);
    CHECK(label->text().startsWith("Select"));
    CHECK(label->text().endsWith("Move"));
    scene.action("selection.mode.faces")->trigger();
    scene.action("gizmo.mode.rotate")->trigger();
    CHECK(label->text() == QString("Select  \u00b7  Faces  \u00b7  Rotate"));
    CHECK(ui::modeSummary(session) == label->text());
    REQUIRE(session.perform("tool.entity"));
    CHECK(label->text().startsWith("Entity"));
}

TEST_CASE("transform actions route through the session, menus, toolbar and status bar") {
    TransformScene scene;
    auto& session = scene.session();
    const auto* edit = scene.window.findChild<QMenu*>("menuTransform");
    const auto* gizmo = scene.window.findChild<QMenu*>("menuGizmo");
    REQUIRE(edit);
    REQUIRE(gizmo);
    for (const char* id : {"transform.dialog", "edit.flip.x", "edit.flip.y", "edit.flip.z", "nudge.left", "nudge.right",
                           "nudge.up", "nudge.down", "transform.texture_lock.toggle"})
        CHECK(edit->actions().contains(scene.action(id)));
    for (const char* id : {"gizmo.mode.translate", "gizmo.mode.rotate", "gizmo.mode.scale", "gizmo.space.toggle",
                           "gizmo.pivot.cycle", "snap.mode.toggle"})
        CHECK(gizmo->actions().contains(scene.action(id)));

    auto* toolbar = scene.window.findChild<QToolBar*>("toolbarTransform");
    REQUIRE(toolbar);
    for (const char* id : {"gizmo.mode.translate", "gizmo.mode.rotate", "gizmo.mode.scale", "snap.mode.toggle",
                           "transform.texture_lock.toggle"})
        CHECK(toolbar->actions().contains(scene.action(id)));
    for (const char* name : {"statusSnapMode", "statusTextureLock"})
        CHECK(scene.window.findChild<QToolButton*>(name));
    CHECK(scene.window.findChild<QLabel*>("statusGrid"));

    // Gizmo modes: exclusive, mirrored on toolbar action and status button.
    CHECK(scene.action("gizmo.mode.translate")->isChecked());
    scene.action("gizmo.mode.rotate")->trigger();
    CHECK(session.gizmoMode() == editor::GizmoMode::Rotate);
    CHECK(scene.action("gizmo.mode.rotate")->isChecked());
    CHECK_FALSE(scene.action("gizmo.mode.translate")->isChecked());
    CHECK(scene.window.findChild<QLabel*>("statusMode")->text().contains("Rotate"));
    scene.action("gizmo.mode.scale")->trigger();
    CHECK(session.gizmoMode() == editor::GizmoMode::Scale);
    CHECK(scene.window.findChild<QLabel*>("statusMode")->text().contains("Scale"));
    scene.action("gizmo.mode.translate")->trigger();

    // Snap mode and texture lock toggles.
    CHECK(session.snapSettings().mode == editor::SnapMode::Absolute);
    scene.action("snap.mode.toggle")->trigger();
    CHECK(session.snapSettings().mode == editor::SnapMode::Relative);
    CHECK(scene.action("snap.mode.toggle")->isChecked());
    CHECK(scene.action("snap.mode.toggle")->text().contains("Relative"));
    scene.action("snap.mode.toggle")->trigger();
    CHECK(session.snapSettings().mode == editor::SnapMode::Absolute);
    const bool lock = session.textureLock();
    scene.window.findChild<QToolButton*>("statusTextureLock")->click();
    CHECK(session.textureLock() == !lock);
    CHECK(scene.action("transform.texture_lock.toggle")->isChecked() == !lock);
    scene.action("gizmo.space.toggle")->trigger();
    CHECK(session.gizmoSpace() == editor::GizmoSpace::Local);
    CHECK(scene.action("gizmo.space.toggle")->isChecked());
    const auto pivot = session.pivotMode();
    scene.action("gizmo.pivot.cycle")->trigger();
    CHECK(session.pivotMode() != pivot);

    // Document edits: one undo step each.
    const auto before = scene.undo();
    scene.action("edit.flip.x")->trigger();
    CHECK(scene.undo() == before + 1);
    scene.action("nudge.right")->trigger();
    CHECK(scene.undo() == before + 2);

    // Hammer binds W/E/R to the pane and Ctrl+M to the window; bare modifiers never become shortcuts.
    CHECK(scene.action("transform.dialog")->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_M));
    CHECK(scene.action("gizmo.mode.rotate")->shortcutContext() == Qt::WidgetShortcut);
}

TEST_CASE("transform dialog applies exact values and rejects invalid input") {
    TransformScene scene;
    auto& session = scene.session();
    scene.action("transform.dialog")->trigger();
    auto* dialog = scene.window.findChild<ui::TransformDialog*>("transformDialog");
    REQUIRE(dialog);
    const auto field = [&](const char* name) {
        auto* f = dialog->findChild<QLineEdit*>(name);
        REQUIRE(f);
        return f;
    };
    auto* error = dialog->findChild<QLabel*>("transformError");
    REQUIRE(error);

    // Absolute move: the pivot goes to the typed point. Box is 64 wide, so min = pivot - 32.
    const auto before = scene.undo();
    dialog->setTab(ui::TransformDialog::Move);
    field("moveX")->setText("100");
    field("moveY")->setText("200");
    field("moveZ")->setText("300");
    CHECK(dialog->apply());
    CHECK(scene.undo() == before + 1);
    editor_test::checkPoint(scene.min(), {68, 168, 268});

    // Relative move.
    dialog->findChild<QCheckBox*>("moveRelative")->setChecked(true);
    field("moveX")->setText("8");
    field("moveY")->setText("0");
    field("moveZ")->setText("-4");
    CHECK(dialog->apply());
    CHECK(scene.undo() == before + 2);
    editor_test::checkPoint(scene.min(), {76, 168, 264});

    // Invalid input: error shown, nothing changes.
    field("moveX")->setText("abc");
    CHECK_FALSE(dialog->apply());
    CHECK_FALSE(dialog->error().isEmpty());
    CHECK(scene.undo() == before + 2);
    field("moveX")->setText("nan");
    CHECK_FALSE(dialog->apply());
    field("moveX")->setText("1e999");
    CHECK_FALSE(dialog->apply());
    CHECK(scene.undo() == before + 2);
    editor_test::checkPoint(scene.min(), {76, 168, 264});

    // Rotate: yaw 90 about the pivot keeps a cube's extents but is one undo step.
    dialog->setTab(ui::TransformDialog::Rotate);
    field("rotateYaw")->setText("45");
    CHECK(dialog->apply());
    CHECK(scene.undo() == before + 3);
    CHECK(dialog->error().isEmpty());

    // Scale: zero is rejected, 2 doubles the box.
    dialog->setTab(ui::TransformDialog::Scale);
    field("scaleX")->setText("0");
    CHECK_FALSE(dialog->apply());
    CHECK(scene.undo() == before + 3);
    field("scaleX")->setText("2");
    field("scaleY")->setText("2");
    field("scaleZ")->setText("2");
    CHECK(dialog->apply());
    CHECK(scene.undo() == before + 4);
    for (int i = 0; i < 4; ++i) scene.session().perform("edit.undo");
    CHECK(scene.undo() == before);
    editor_test::checkPoint(scene.min(), {5, 5, 5});
    (void)session;
    dialog->reject();
}

TEST_CASE("simulated gizmo drag commits one undo step identical to the editor-level drag") {
    TransformScene scene;
    auto& session = scene.session();
    const auto context = session.gizmoContext();
    REQUIRE(context);
    const auto camera = cameraLooking(context->pivot + DVec3{-260, -300, 220}, context->pivot);
    const auto handle = findHandle(session, camera, editor::GizmoHandle::X);
    REQUIRE(handle);

    ui::TransformInput input;
    input.setSession(&session);
    const auto before = scene.undo();
    const auto start = scene.min();

    auto move = mouseEvent(QEvent::MouseMove, *handle, Qt::NoButton, Qt::NoButton);
    CHECK(input.mouse(move, camera, kPane));
    CHECK(input.hovered() == editor::GizmoHandle::X);
    auto press = mouseEvent(QEvent::MouseButtonPress, *handle, Qt::LeftButton, Qt::LeftButton);
    CHECK(input.mouse(press, camera, kPane));
    CHECK(input.owns());
    CHECK(session.transformActive());

    // Drag along the projected X axis.
    const QPointF end = *handle + QPointF(70, 25);
    ui::TransformInput* in = &input;
    auto drag = mouseEvent(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    CHECK(in->mouse(drag, camera, kPane));
    CHECK_FALSE(session.preview().empty());
    CHECK(scene.undo() == before);  // Previewing never touches the document.
    CHECK_FALSE(session.activeReadout().text.empty());
    auto release = mouseEvent(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    CHECK(in->mouse(release, camera, kPane));
    CHECK_FALSE(input.owns());
    CHECK(session.preview().empty());
    CHECK(scene.undo() == before + 1);
    const auto moved = scene.min();
    CHECK(moved.x != start.x);

    // The same drag at the editor level on a fresh document yields the same result.
    TransformScene reference;
    auto& ref = reference.session();
    const auto refContext = ref.gizmoContext();
    REQUIRE(refContext);
    const auto view = ui::gizmoViewFor(camera, kPane, refContext->pivot);
    REQUIRE(ref.beginGizmoDrag(editor::GizmoHandle::X, view, rayAtPixel(camera, *handle)));
    REQUIRE(ref.updateGizmoDrag(rayAtPixel(camera, end), ui::transformModifiers(ref.preset(), Qt::NoModifier)));
    REQUIRE(ref.endGizmoDrag());
    editor_test::checkPoint(moved, reference.min());

    // Undo restores the start in one step.
    session.perform("edit.undo");
    editor_test::checkPoint(scene.min(), start);

    // Esc cancels a drag without an undo step; right press too.
    const auto press2 = mouseEvent(QEvent::MouseButtonPress, *handle, Qt::LeftButton, Qt::LeftButton);
    REQUIRE(input.mouse(press2, camera, kPane));
    const auto drag2 = mouseEvent(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    input.mouse(drag2, camera, kPane);
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    CHECK(input.key(esc, camera, kPane, true));
    CHECK_FALSE(session.transformActive());
    CHECK(session.preview().empty());
    editor_test::checkPoint(scene.min(), start);
    const auto after = scene.undo();
    const auto press3 = mouseEvent(QEvent::MouseButtonPress, *handle, Qt::LeftButton, Qt::LeftButton);
    REQUIRE(input.mouse(press3, camera, kPane));
    auto right = mouseEvent(QEvent::MouseButtonPress, end, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
    CHECK(input.mouse(right, camera, kPane));
    CHECK_FALSE(session.transformActive());
    CHECK(scene.undo() == after);

    // A press away from the gizmo is not consumed (selection click logic unchanged).
    auto away = mouseEvent(QEvent::MouseButtonPress, QPointF(5, 5), Qt::LeftButton, Qt::LeftButton);
    CHECK_FALSE(input.mouse(away, camera, kPane));
    CHECK_FALSE(session.transformActive());
}

TEST_CASE("snap suspend follows the preset and the overlay describes the gizmo") {
    app();
    const auto& hammer = editor::hammerPreset();
    const auto& blender = editor::blenderPreset();
    CHECK(ui::snapSuspendModifier(hammer) == Qt::AltModifier);
    CHECK(ui::snapSuspendModifier(blender) == Qt::ControlModifier);
    CHECK(ui::transformModifiers(hammer, Qt::AltModifier).suspend);
    CHECK_FALSE(ui::transformModifiers(hammer, Qt::ControlModifier).suspend);
    CHECK(ui::transformModifiers(blender, Qt::ControlModifier).suspend);
    CHECK_FALSE(ui::transformModifiers(blender, Qt::AltModifier).suspend);
    CHECK(ui::transformModifiers(hammer, Qt::ShiftModifier).fine);

    TransformScene scene;
    auto& session = scene.session();
    const auto context = session.gizmoContext();
    REQUIRE(context);
    const auto camera = cameraLooking(context->pivot + DVec3{-260, -300, 220}, context->pivot);
    ui::TransformInput input;
    input.setSession(&session);
    for (const auto mode : {editor::GizmoMode::Translate, editor::GizmoMode::Rotate, editor::GizmoMode::Scale}) {
        session.setGizmoMode(mode);
        render::DrawList list;
        input.appendOverlay(list, camera, kPane, 1.0);
        CHECK((list.overlaySegments.size() + list.overlayTriangles.size()) > 0);
        CHECK(list.viewportSize.x == doctest::Approx(800.0));
    }
    // Ortho panes draw the gizmo too.
    viewport::Camera ortho = cameraLooking(context->pivot + DVec3{0, 0, 500}, context->pivot);
    ortho.projection = viewport::ProjectionKind::Orthographic;
    session.setGizmoMode(editor::GizmoMode::Translate);
    render::DrawList list;
    input.appendOverlay(list, ortho, kPane, 1.0);
    CHECK_FALSE(list.overlaySegments.empty());
    // Nothing selected: no gizmo.
    session.selection().clear();
    render::DrawList empty;
    input.appendOverlay(empty, camera, kPane, 1.0);
    CHECK(empty.overlaySegments.empty());
    CHECK(empty.overlayTriangles.empty());
}

TEST_CASE("Blender modal transform: G, axis, digits, Enter from synthetic keys") {
    TransformScene scene("Blender");
    auto& session = scene.session();
    session.setPreset(editor::blenderPreset());
    const auto context = session.gizmoContext();
    REQUIRE(context);
    const auto camera = cameraLooking(context->pivot + DVec3{-260, -300, 220}, context->pivot);
    ui::TransformInput input;
    input.setSession(&session);
    const auto start = scene.min();
    const auto before = scene.undo();

    auto hover = mouseEvent(QEvent::MouseMove, QPointF(400, 300), Qt::NoButton, Qt::NoButton);
    input.mouse(hover, camera, kPane);
    const auto key = [&](int k, Qt::KeyboardModifiers m = Qt::NoModifier) {
        QKeyEvent e(QEvent::KeyPress, k, m, QString());
        return input.key(e, camera, kPane, true);
    };
    CHECK(key(Qt::Key_G));
    CHECK(session.modal().active());
    CHECK(input.owns());
    CHECK(key(Qt::Key_X));
    CHECK(key(Qt::Key_5));
    CHECK(key(Qt::Key_0));
    CHECK(session.activeReadout().text.size() > 0);
    CHECK(key(Qt::Key_Return));
    CHECK_FALSE(session.transformActive());
    CHECK(scene.undo() == before + 1);
    editor_test::checkPoint(scene.min(), start + DVec3{50, 0, 0});

    // Left click commits, right click cancels, Esc cancels.
    CHECK(key(Qt::Key_G));
    input.mouse(mouseEvent(QEvent::MouseMove, QPointF(430, 300), Qt::NoButton, Qt::NoButton), camera, kPane);
    CHECK(input.mouse(mouseEvent(QEvent::MouseButtonPress, QPointF(430, 300), Qt::LeftButton, Qt::LeftButton), camera, kPane));
    CHECK_FALSE(session.transformActive());
    CHECK(scene.undo() == before + 2);

    CHECK(key(Qt::Key_R));
    CHECK(input.mouse(mouseEvent(QEvent::MouseButtonPress, QPointF(430, 300), Qt::RightButton, Qt::RightButton), camera, kPane));
    CHECK_FALSE(session.transformActive());
    CHECK(scene.undo() == before + 2);
    CHECK(key(Qt::Key_S));
    CHECK(key(Qt::Key_Escape));
    CHECK_FALSE(session.transformActive());
    CHECK(scene.undo() == before + 2);

    // Keys unrelated to modal start are not consumed.
    CHECK_FALSE(key(Qt::Key_Q));
}

#include <QClipboard>
#include <QMimeData>
#include <QStyle>
#include <QStyleOptionViewItem>
#include "ui/paste_special_dialog.hpp"
#include "ui/tie_dialog.hpp"

namespace {
using namespace apex;

void setClipboardText(const QString& text) {
    auto* mime = new QMimeData;
    mime->setText(text);
    QApplication::clipboard()->setMimeData(mime);
}

void closeDialog(QWidget* dialog) {
    dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

DVec3 solidMin(document::Document& doc, document::NodeId id) {
    const auto handle = doc.handle(id);
    REQUIRE(handle);
    const auto solid = geometry::solidGeometry(*doc.get(*handle)->block);
    REQUIRE(solid);
    DVec3 m{1e9};
    for (const auto& v : solid->poly.vertices) m = glm::min(m, v);
    return m;
}

std::size_t nodeCount(const ui::MainWindow& w) { return editor_test::nodes(*w.document()).size(); }
}  // namespace

TEST_CASE("copy and paste round-trip through the system clipboard") {
    TransformScene scene;
    auto& session = scene.session();
    const auto nodes = nodeCount(scene.window);
    const auto undo = scene.undo();

    scene.action("edit.copy")->trigger();
    const auto* mime = QApplication::clipboard()->mimeData();
    REQUIRE(mime);
    CHECK(mime->hasFormat(ui::MainWindow::fragmentMimeType()));
    CHECK(mime->hasText());
    CHECK(mime->data(ui::MainWindow::fragmentMimeType()).toStdString() == mime->text().toStdString());
    CHECK(scene.undo() == undo);
    CHECK(scene.action("edit.paste")->isEnabled());

    // No pointer over a pane: paste in place keeps the copied coordinates exactly.
    scene.action("edit.paste")->trigger();
    CHECK(scene.undo() == undo + 1);
    CHECK(nodeCount(scene.window) == nodes + 1);
    auto selected = editor::selectedNodes(session.selection());
    REQUIRE(selected.size() == 1);
    CHECK(selected[0] != scene.boxId);
    editor_test::checkPoint(solidMin(*scene.window.document(), selected[0]), {5, 5, 5});
    scene.window.document()->undo();
    CHECK(nodeCount(scene.window) == nodes);

    // Pointer over a pane looking straight down at the ground: the copy is centred on the hit and
    // rests on the plane.
    scene.window.viewportArea()->setPointerRayOverride(Ray{{640, 320, 500}, {0, 0, -1}});
    session.selectNode(scene.boxId);
    scene.action("edit.paste")->trigger();
    selected = editor::selectedNodes(session.selection());
    REQUIRE(selected.size() == 1);
    const auto placed = solidMin(*scene.window.document(), selected[0]);
    CHECK(std::abs(placed.z) < 1e-6);
    CHECK(std::abs(placed.x + 32 - 640) <= 32.0 + 1e-6);
    CHECK(std::abs(placed.y + 32 - 320) <= 32.0 + 1e-6);
    CHECK(placed != DVec3{5, 5, 5});
    scene.window.document()->undo();

    // A ray that misses the plane (parallel) falls back to in place.
    scene.window.viewportArea()->setPointerRayOverride(Ray{{0, 0, 500}, {1, 0, 0}});
    scene.action("edit.paste")->trigger();
    selected = editor::selectedNodes(session.selection());
    REQUIRE(selected.size() == 1);
    editor_test::checkPoint(solidMin(*scene.window.document(), selected[0]), {5, 5, 5});
    scene.window.document()->undo();
    scene.window.viewportArea()->setPointerRayOverride(std::nullopt);

    // Paste in place ignores the pointer.
    scene.window.viewportArea()->setPointerRayOverride(Ray{{640, 320, 500}, {0, 0, -1}});
    scene.action("edit.paste_in_place")->trigger();
    selected = editor::selectedNodes(session.selection());
    REQUIRE(selected.size() == 1);
    editor_test::checkPoint(solidMin(*scene.window.document(), selected[0]), {5, 5, 5});
    scene.window.viewportArea()->setPointerRayOverride(std::nullopt);
}

TEST_CASE("paste reads plain text, other instances' custom MIME, and ignores everything else") {
    TransformScene scene;
    auto& session = scene.session();
    scene.action("edit.copy")->trigger();
    const QString text = QApplication::clipboard()->mimeData()->text();
    const auto undo = scene.undo();

    // Plain text only (a text editor, or a tool that drops the custom type).
    setClipboardText(text);
    CHECK(scene.action("edit.paste")->isEnabled());
    scene.action("edit.paste_in_place")->trigger();
    CHECK(scene.undo() == undo + 1);

    // Custom MIME only (another Apex instance).
    auto* mime = new QMimeData;
    mime->setData(ui::MainWindow::fragmentMimeType(), text.toUtf8());
    QApplication::clipboard()->setMimeData(mime);
    CHECK(ui::MainWindow::clipboardFragment() == text.toStdString());
    scene.action("edit.paste")->trigger();
    CHECK(scene.undo() == undo + 2);

    // Not a fragment: ignored with a status message, nothing changes, paste is disabled.
    for (const QString junk : {QStringLiteral("hello world"), QStringLiteral("versioninfo { \"editorversion\" \"400\" }"),
                               QString()}) {
        setClipboardText(junk);
        CHECK(ui::MainWindow::clipboardFragment().empty());
        CHECK_FALSE(scene.action("edit.paste")->isEnabled());
        CHECK_FALSE(scene.action("edit.paste_in_place")->isEnabled());
        CHECK_FALSE(scene.action("edit.paste_special")->isEnabled());
        scene.action("edit.paste")->trigger();
        CHECK(scene.undo() == undo + 2);
    }
    (void)session;
}

TEST_CASE("cut writes the clipboard and one undo restores the document") {
    TransformScene scene;
    const auto before = scene.window.document()->write();
    REQUIRE(before);
    const auto undo = scene.undo();
    scene.action("edit.cut")->trigger();
    CHECK(scene.undo() == undo + 1);
    CHECK(QApplication::clipboard()->mimeData()->hasFormat(ui::MainWindow::fragmentMimeType()));
    CHECK(scene.session().selection().ordered().empty());
    CHECK_FALSE(scene.action("edit.cut")->isEnabled());
    scene.window.document()->undo();
    const auto after = scene.window.document()->write();
    REQUIRE(after);
    CHECK(*after == *before);
}

TEST_CASE("paste special dialog applies copies and rejects invalid input") {
    TransformScene scene;
    scene.action("edit.copy")->trigger();
    scene.action("edit.paste_special")->trigger();
    auto* dialog = scene.window.findChild<ui::PasteSpecialDialog*>("pasteSpecialDialog");
    REQUIRE(dialog);
    const auto field = [&](const char* name) {
        auto* f = dialog->findChild<QLineEdit*>(name);
        REQUIRE(f);
        return f;
    };
    const auto undo = scene.undo();
    const auto nodes = nodeCount(scene.window);
    const auto reject = [&](const char* name, const char* value) {
        const QString old = field(name)->text();
        field(name)->setText(value);
        CHECK_FALSE(dialog->apply());
        CHECK_FALSE(dialog->error().isEmpty());
        CHECK(scene.undo() == undo);
        CHECK(nodeCount(scene.window) == nodes);
        field(name)->setText(old);
    };
    reject("psCopies", "0");
    reject("psCopies", "257");
    reject("psCopies", "abc");
    reject("psCopies", "1.5");
    reject("psOffsetX", "nan");
    reject("psOffsetY", "1e999");
    reject("psOffsetZ", "");
    reject("psPitch", "x");
    reject("psYaw", "inf");
    reject("psRoll", "1,5");
    {
        auto* mode = dialog->findChild<QComboBox*>("psNameMode");
        REQUIRE(mode);
        mode->setCurrentIndex(1);  // Prefix with no text.
        CHECK_FALSE(dialog->apply());
        CHECK(scene.undo() == undo);
        mode->setCurrentIndex(0);
    }

    // Three copies, 128 units apart in x: one transaction.
    field("psCopies")->setText("3");
    field("psOffsetX")->setText("128");
    CHECK(dialog->apply());
    CHECK(dialog->error().isEmpty());
    CHECK(scene.undo() == undo + 1);
    CHECK(nodeCount(scene.window) == nodes + 3);
    const auto selected = editor::selectedNodes(scene.session().selection());
    REQUIRE(selected.size() == 3);
    std::vector<double> xs;
    for (const auto id : selected) xs.push_back(solidMin(*scene.window.document(), id).x);
    std::ranges::sort(xs);
    CHECK(xs[1] - xs[0] == doctest::Approx(128));
    CHECK(xs[2] - xs[1] == doctest::Approx(128));
    scene.window.document()->undo();

    // Grouping the copies adds groups.
    dialog->findChild<QCheckBox*>("psGroup")->setChecked(true);
    CHECK(dialog->apply());
    CHECK(nodeCount(scene.window) > nodes + 3);
    scene.window.document()->undo();
    closeDialog(dialog);
}

TEST_CASE("delete, duplicate, group, ungroup, tie and move to world are undoable actions") {
    TransformScene scene;
    auto& doc = *scene.window.document();
    auto& session = scene.session();
    const auto start = doc.write();
    REQUIRE(start);
    const auto undo = scene.undo();
    const auto nodes = nodeCount(scene.window);

    scene.action("edit.duplicate")->trigger();
    CHECK(scene.undo() == undo + 1);
    CHECK(nodeCount(scene.window) == nodes + 1);
    session.perform("edit.undo");

    scene.action("edit.delete")->trigger();
    CHECK(scene.undo() == undo + 1);
    CHECK(nodeCount(scene.window) == nodes - 1);
    CHECK_FALSE(scene.action("edit.delete")->isEnabled());
    session.perform("edit.undo");
    CHECK(*doc.write() == *start);
    CHECK(scene.action("edit.delete")->isEnabled());

    // Group and ungroup.
    session.perform("selection.mode.objects");
    session.selectNode(scene.boxId);
    scene.action("edit.group")->trigger();
    CHECK(scene.undo() == undo + 1);
    CHECK(nodeCount(scene.window) > nodes);
    scene.action("edit.ungroup")->trigger();
    CHECK(scene.undo() == undo + 2);
    CHECK(nodeCount(scene.window) == nodes);
    session.perform("edit.undo");
    session.perform("edit.undo");
    CHECK(*doc.write() == *start);

    // Tie to entity through the dialog, then move back to the world.
    session.perform("selection.mode.solids");
    session.selectNode(scene.boxId);
    scene.action("edit.tie_to_entity")->trigger();
    auto* tie = scene.window.findChild<ui::TieDialog*>("tieDialog");
    REQUIRE(tie);
    CHECK(tie->className() == "func_detail");
    tie->setClassName("bad class");
    CHECK_FALSE(tie->apply());
    CHECK_FALSE(tie->error().isEmpty());
    CHECK(scene.undo() == undo);
    tie->setClassName("func_detail");
    CHECK(tie->apply());
    CHECK(scene.undo() == undo + 1);
    CHECK(editor::brushEntityOf(doc, scene.boxId));
    closeDialog(tie);
    session.perform("selection.mode.solids");
    session.selectNode(scene.boxId);
    scene.action("edit.move_to_world")->trigger();
    CHECK(scene.undo() == undo + 2);
    CHECK_FALSE(editor::brushEntityOf(doc, scene.boxId));
    session.perform("edit.undo");
    CHECK(editor::brushEntityOf(doc, scene.boxId));
    session.perform("edit.undo");
    CHECK(*doc.write() == *start);
}

TEST_CASE("edit menu enabled states follow selection and clipboard") {
    TransformScene scene;
    auto& session = scene.session();
    const auto* edit = scene.window.findChild<QMenu*>("menuEdit");
    REQUIRE(edit);
    const char* selectionActions[] = {"edit.copy", "edit.cut", "edit.delete", "edit.duplicate", "edit.group",
                                      "edit.ungroup", "edit.tie_to_entity", "edit.move_to_world"};
    for (const char* id : {"edit.copy", "edit.cut", "edit.paste", "edit.paste_in_place", "edit.paste_special",
                           "edit.duplicate", "edit.delete", "edit.group", "edit.ungroup", "edit.tie_to_entity",
                           "edit.move_to_world"})
        CHECK(edit->actions().contains(scene.action(id)));
    setClipboardText(QString());
    session.perform("selection.none");
    QCoreApplication::processEvents();
    for (const char* id : selectionActions) CHECK_FALSE(scene.action(id)->isEnabled());
    session.selectNode(scene.boxId);
    for (const char* id : selectionActions) CHECK(scene.action(id)->isEnabled());
    CHECK_FALSE(scene.action("edit.paste")->isEnabled());
    scene.action("edit.copy")->trigger();
    CHECK(scene.action("edit.paste")->isEnabled());
    // Faces mode has no objects to copy.
    session.perform("selection.mode.faces");
    CHECK_FALSE(scene.action("edit.copy")->isEnabled());
    CHECK(scene.action("edit.paste")->isEnabled());
    // Hammer: Delete is a plain key (pane), Ctrl+C/V are window shortcuts.
    CHECK(scene.action("edit.delete")->shortcutContext() == Qt::WidgetShortcut);
    CHECK(scene.action("edit.copy")->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_C));
    CHECK(scene.action("edit.paste")->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_V));
}

TEST_CASE("quad splitter bars stay aligned and the crossing drags both axes") {
    auto& a = app();
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
    using namespace apex;
    ui::ViewportArea area;
    area.resize(800, 600);
    area.show();
    area.flush();
    QSplitter* root = nullptr;
    std::vector<QSplitter*> rows;
    for (auto* s : area.findChildren<QSplitter*>()) {
        if (qobject_cast<QSplitter*>(s->parentWidget())) rows.push_back(s);
        else root = s;
    }
    REQUIRE(root);
    REQUIRE(rows.size() == 2);

    const auto drag = [&](QSplitter* s, QPoint localStart, QPoint delta, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        auto* h = s->handle(1);
        const QPointF start = h->mapToGlobal(QPointF(localStart));
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(localStart), start, Qt::LeftButton, Qt::LeftButton, mods);
        QCoreApplication::sendEvent(h, &press);
        for (int step = 1; step <= 4; ++step) {
            const QPointF at = start + QPointF(delta) * (step / 4.0);
            QMouseEvent move(QEvent::MouseMove, h->mapFromGlobal(at), at, Qt::NoButton, Qt::LeftButton, mods);
            QCoreApplication::sendEvent(h, &move);
        }
        const QPointF end = start + QPointF(delta);
        QMouseEvent release(QEvent::MouseButtonRelease, h->mapFromGlobal(end), end, Qt::LeftButton, Qt::NoButton, mods);
        QCoreApplication::sendEvent(h, &release);
    };
    const auto ratio = [](QSplitter* s) {
        const auto sizes = s->sizes();
        return static_cast<double>(sizes[0]) / (sizes[0] + sizes[1]);
    };

    CHECK(std::abs(ratio(rows[0]) - ratio(rows[1])) < 0.01);
    // A bar of one row drags its sibling's bar with it.
    drag(rows[0], rows[0]->handle(1)->rect().center(), QPoint(80, 0));
    CHECK(ratio(rows[0]) > 0.55);
    CHECK(std::abs(ratio(rows[0]) - ratio(rows[1])) < 0.01);
    // Shift drags the one bar alone.
    drag(rows[0], rows[0]->handle(1)->rect().center(), QPoint(-120, 0), Qt::ShiftModifier);
    CHECK(ratio(rows[0]) < ratio(rows[1]) - 0.1);
    // Dropping the crossing: both axes move and every bar ends up aligned again.
    const double vertical = ratio(root);
    const auto crossing = root->handle(1)->mapFromGlobal(rows[0]->mapToGlobal(rows[0]->handle(1)->geometry().center()));
    drag(root, crossing, QPoint(50, 60));
    CHECK(ratio(root) > vertical);
    CHECK(std::abs(ratio(rows[0]) - ratio(rows[1])) < 0.01);
    CHECK(area.viewportLayout().tree().ratio == doctest::Approx(ratio(root)).epsilon(0.01));
}

#include "ui/area_frame.hpp"
#include "ui/inspector_panel.hpp"
#include "ui/workspace_bar.hpp"
#include "ui/visgroup_panel.hpp"
#include "ui/history_panel.hpp"
#include "editor/visibility.hpp"
#include <QListWidget>
#include <QTreeWidgetItemIterator>
#include <QDrag>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QMimeData>
#include <QStyle>
#include <QStyleOptionViewItem>

namespace {
QTreeWidgetItem* visgroupItem(ui::VisgroupPanel& panel, int id) {
    for (QTreeWidgetItemIterator it(&panel); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).isValid() && (*it)->data(0, Qt::UserRole).toInt() == id) return *it;
    return nullptr;
}
void panelAction(ui::VisgroupPanel& panel, const char* id) {
    auto* action = panel.findChild<QAction*>(QString::fromLatin1(id));
    REQUIRE(action);
    REQUIRE(action->isEnabled());
    action->trigger();
    QCoreApplication::processEvents();
}
}

TEST_CASE("visgroup panel edits nested groups membership and visibility") {
    app();
    document::Document doc(editor_test::parsed(R"(visgroups {
        visgroup { name Parent visgroupid 10 color "12 34 56"
            visgroup { name Child visgroupid 11 color "56 34 12" } }
    } world { id 1 } entity { id 2 classname light editor { visgroupid 11 } })"));
    editor::EditorSession session;
    session.attach(doc);
    ui::VisgroupPanel panel;
    panel.setDocument(&doc);
    panel.setEditorSession(&session);
    panel.resize(320, 500);
    panel.show();
    QCoreApplication::processEvents();
    REQUIRE(panel.topLevelItemCount() == 2);
    CHECK(panel.topLevelItem(0)->text(0) == "User");
    CHECK(panel.topLevelItem(1)->text(0) == "Auto");
    CHECK(panel.topLevelItem(1)->childCount() == static_cast<int>(editor::autoVisgroups().size()));
    auto* child = visgroupItem(panel, 11);
    REQUIRE(child);
    CHECK(child->parent() == visgroupItem(panel, 10));
    CHECK_FALSE(child->icon(0).isNull());
    CHECK(child->checkState(0) == Qt::Checked);
    const auto member = editor::autoMembers(doc, "lights").front();
    const auto before = doc.write().value();
    child->setCheckState(0, Qt::Unchecked);
    CHECK_FALSE(editor::isVisible(doc, member));
    QCoreApplication::processEvents();
    CHECK(visgroupItem(panel, 11)->checkState(0) == Qt::Unchecked);
    REQUIRE(session.perform("edit.undo"));
    CHECK(doc.write().value() == before);
    QCoreApplication::processEvents();
    CHECK(visgroupItem(panel, 11)->checkState(0) == Qt::Checked);

    // Auto check state follows the session, including undo and overlapping groups.
    const auto autoItem = [&](std::string_view id) {
        const auto groups = editor::autoVisgroups();
        for (std::size_t i = 0; i < groups.size(); ++i)
            if (groups[i].id == id) return panel.topLevelItem(1)->child(static_cast<int>(i));
        return static_cast<QTreeWidgetItem*>(nullptr);
    };
    autoItem("lights")->setCheckState(0, Qt::Unchecked);
    CHECK_FALSE(editor::isVisible(doc, member));
    REQUIRE(session.setAutoVisgroupShown("point-entities", false));
    QCoreApplication::processEvents();
    autoItem("lights")->setCheckState(0, Qt::Checked);
    QCoreApplication::processEvents();
    CHECK_FALSE(editor::isVisible(doc, member));
    CHECK(autoItem("lights")->checkState(0) == Qt::Checked);
    REQUIRE(session.perform("edit.undo"));
    QCoreApplication::processEvents();
    CHECK(autoItem("lights")->checkState(0) == Qt::Unchecked);
    REQUIRE(session.unhideAll());
    QCoreApplication::processEvents();

    panel.setCurrentItem(visgroupItem(panel, 10));
    visgroupItem(panel, 10)->setExpanded(false);
    REQUIRE(session.renameVisgroup(11, "Child renamed"));
    QCoreApplication::processEvents();
    CHECK(panel.currentVisgroup() == 10);
    CHECK_FALSE(visgroupItem(panel, 10)->isExpanded());
    panelAction(panel, "visgroup.new_child");
    const int created = panel.currentVisgroup().value();
    CHECK(visgroupItem(panel, created)->parent() == visgroupItem(panel, 10));
    REQUIRE(panel.findChild<QLineEdit*>());
    panelAction(panel, "visgroup.rename");
    // Complete the same inline edit used by Rename.
    panel.currentItem()->setText(0, "New name");
    QCoreApplication::processEvents();
    CHECK(editor::buildVisgroupTree(doc).find(created)->name == "New name");
    session.selectNode(member);
    panelAction(panel, "visgroup.add");
    CHECK(editor::buildVisgroupTree(doc).members(created) == std::vector<document::NodeId>{member});
    session.perform("selection.none");
    panelAction(panel, "visgroup.select");
    CHECK(session.selection().contains(member));
    panelAction(panel, "visgroup.remove");
    CHECK(editor::buildVisgroupTree(doc).members(created).empty());
    // Deliver a drop to the tree viewport, through the same MIME path as a drag.
    visgroupItem(panel, 10)->setExpanded(true);
    QCoreApplication::processEvents();
    const auto drop = [&](int id, QPoint pos) {
        QMimeData mime;
        mime.setData("application/x-apex-visgroup", QByteArray::number(id));
        QDragEnterEvent enter(pos, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(panel.viewport(), &enter);
        QDragMoveEvent move(pos, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(panel.viewport(), &move);
        QDropEvent event(QPointF(pos), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(panel.viewport(), &event);
        CHECK(event.isAccepted());
        QCoreApplication::processEvents();
    };
    drop(created, panel.visualItemRect(visgroupItem(panel, 11)).center());
    QCoreApplication::processEvents();
    CHECK(visgroupItem(panel, created)->parent() == visgroupItem(panel, 11));
    CHECK_FALSE(panel.moveVisgroup(10, created));
    drop(created, QPoint(5, panel.viewport()->height() - 5));
    QCoreApplication::processEvents();
    CHECK(visgroupItem(panel, created)->parent() == panel.topLevelItem(0));
    panelAction(panel, "visgroup.delete");
    CHECK_FALSE(editor::buildVisgroupTree(doc).find(created));
    REQUIRE(session.perform("edit.undo"));
    QCoreApplication::processEvents();
    CHECK(visgroupItem(panel, created));
    panelAction(panel, "visgroup.new");
    const auto top = panel.currentVisgroup().value();
    CHECK(visgroupItem(panel, top)->parent() == panel.topLevelItem(0));
    panel.setEditorSession(nullptr);
    panel.setDocument(nullptr);
    CHECK(panel.topLevelItemCount() == 0);
}

TEST_CASE("history panel lists chronological entries and jumps through undo and redo") {
    app();
    document::Document doc(editor_test::parsed("world { id 1 }"));
    editor::EditorSession session;
    session.attach(doc);
    ui::HistoryPanel panel;
    panel.setDocument(&doc);
    panel.setEditorSession(&session);
    const auto initialMemory = panel.memoryLabel()->text();
    REQUIRE(session.createVisgroup("A"));
    REQUIRE(session.renameVisgroup(1, "B"));
    REQUIRE(session.createVisgroup("C"));
    const auto final = doc.write().value();
    const auto labels = doc.undoLabels();
    REQUIRE(panel.list()->count() == 5);
    for (int i = 0; i < 3; ++i) CHECK(panel.list()->item(i + 1)->text().toStdString() == labels[static_cast<std::size_t>(i)]);
    CHECK(panel.list()->item(4)->font().bold());
    CHECK(panel.list()->item(4)->text().contains("Current state"));
    CHECK(panel.memoryLabel()->text().startsWith("3 entries"));
    CHECK(panel.memoryLabel()->text().contains("KiB"));
    CHECK(panel.memoryLabel()->text() != initialMemory);
    REQUIRE(session.perform("edit.undo"));
    REQUIRE(session.perform("edit.undo"));
    const auto earlier = doc.write().value();
    REQUIRE(session.jumpHistory(3));
    panel.list()->itemClicked(panel.list()->item(1));
    CHECK(doc.undoCount() == 1);
    CHECK(doc.write().value() == earlier);
    CHECK(panel.list()->item(2)->font().bold());
    CHECK(panel.list()->item(3)->text().toStdString() == labels[1]);
    CHECK(panel.list()->item(4)->text().toStdString() == labels[2]);
    panel.list()->itemClicked(panel.list()->item(4));
    CHECK(doc.write().value() == final);
    panel.list()->itemClicked(panel.list()->item(0));
    CHECK(doc.undoCount() == 0);
    CHECK(editor::buildVisgroupTree(doc).nodes.empty());
    panel.setEditorSession(nullptr);
    panel.setDocument(nullptr);
    CHECK(panel.list()->count() == 0);
    CHECK(panel.memoryLabel()->text().startsWith("0 entries"));
}

TEST_CASE("view actions hide unhide isolate and track selection") {
    TransformScene scene;
    auto& session = scene.session();
    auto& doc = *scene.window.document();
    const auto* menu = scene.window.findChild<QMenu*>("menuView");
    REQUIRE(menu);
    for (const char* id : {"view.hide", "view.unhide", "view.isolate", "view.hide_unselected"}) {
        CHECK(menu->actions().contains(scene.action(id)));
        CHECK(scene.action(id)->isEnabled());
    }
    CHECK(scene.action("view.hide")->shortcut() == QKeySequence("H"));
    CHECK(scene.action("view.isolate")->isCheckable());
    const auto before = doc.write().value();
    const auto count = doc.undoCount();
    scene.action("view.isolate")->trigger();
    CHECK(session.viewState().isolate.has_value());
    CHECK(scene.action("view.isolate")->isChecked());
    CHECK(doc.write().value() == before);
    CHECK(doc.undoCount() == count);
    session.perform("selection.none");
    CHECK_FALSE(scene.action("view.hide")->isEnabled());
    CHECK_FALSE(scene.action("view.hide_unselected")->isEnabled());
    CHECK(scene.action("view.isolate")->isEnabled());
    scene.action("view.isolate")->trigger();
    CHECK_FALSE(scene.action("view.isolate")->isChecked());
    CHECK_FALSE(scene.action("view.isolate")->isEnabled());
    CHECK(scene.action("view.hide")->shortcutContext() == Qt::WidgetShortcut);
    session.selectNode(scene.boxId);
    scene.action("view.hide")->trigger();
    CHECK_FALSE(editor::isVisible(doc, scene.boxId));
    CHECK_FALSE(scene.action("view.hide")->isEnabled());
    scene.action("view.unhide")->trigger();
    CHECK(editor::isVisible(doc, scene.boxId));
    session.selectNode(scene.boxId);
    scene.action("view.hide_unselected")->trigger();
    CHECK(editor::isVisible(doc, scene.boxId));
    CHECK(doc.undoCount() > count);
    // Every panel role has a Show area entry that expands its collapsed area.
    auto* area = scene.window.viewportArea();
    for (const auto role : {viewport::AreaRole::Inspector, viewport::AreaRole::Visgroups, viewport::AreaRole::History}) {
        auto* show = scene.window.findChild<QAction*>(QStringLiteral("actionShowArea_") + QString::fromLatin1(viewport::nameOf(role)));
        REQUIRE(show);
        const auto pane = area->viewportLayout().paneIds(role).front();
        area->setCollapsed(pane, true);
        area->flush();
        CHECK(area->viewportLayout().collapsed(pane));
        show->trigger();
        CHECK_FALSE(area->viewportLayout().collapsed(pane));
    }
}

TEST_CASE("areas take any role, collapse, pop out, and workspaces persist") {
    app();
    using viewport::AreaRole;
    QSettings settings;
    settings.remove("ui/workspaces");
    settings.remove("ui/inspectorWindow");
    {
        ui::MainWindow window;
        window.show();
        auto* area = window.viewportArea();
        area->flush();
        for (const char* id : {"view.hide", "view.unhide", "view.isolate", "view.hide_unselected"}) {
            auto* action = window.findChild<QAction*>(id);
            REQUIRE(action);
            CHECK_FALSE(action->isEnabled());
        }
        // The default "Layout" workspace: every panel is an area beside a quad.
        CHECK(window.findChild<QTabBar*>("WorkspaceTabs")->count() == 3);
        CHECK(area->findChildren<ui::PaneFrame*>().size() == 4);
        CHECK(area->findChildren<ui::HistoryPanel*>().size() == 1);
        CHECK(area->findChildren<ui::VisgroupPanel*>().size() == 1);
        CHECK(area->findChildren<ui::InspectorPanel*>().size() == 1);
        REQUIRE(window.outliner());
        REQUIRE(window.logPanel());

        // Collapse keeps the panel widget; the strip expands it again.
        const auto outlinerPane = area->viewportLayout().paneIds(AreaRole::Outliner).front();
        auto* outliner = area->panel(outlinerPane);
        area->setCollapsed(outlinerPane, true);
        area->flush();
        auto strips = area->findChildren<ui::CollapsedStrip*>();
        REQUIRE(strips.size() == 1);
        CHECK(strips.front()->paneId() == outlinerPane);
        CHECK(area->panel(outlinerPane) == outliner);
        CHECK_FALSE(outliner->isVisible());
        emit strips.front()->expandRequested(outlinerPane);
        area->flush();
        CHECK(area->findChildren<ui::CollapsedStrip*>().empty());
        CHECK(area->panel(outlinerPane) == outliner);
        CHECK(outliner->isVisible());

        // Switching a role: the history area becomes a fifth viewport.
        const auto historyPane = area->viewportLayout().paneIds(AreaRole::History).front();
        area->setRole(historyPane, AreaRole::Viewport);
        area->flush();
        CHECK(area->findChildren<ui::PaneFrame*>().size() == 5);
        CHECK(area->findChildren<ui::HistoryPanel*>().empty());
        // Without a history area, showing it opens a floating window.
        window.findChild<QAction*>("actionShowArea_history")->trigger();
        REQUIRE(area->floating(AreaRole::History));
        CHECK(qobject_cast<ui::HistoryPanel*>(area->floating(AreaRole::History)->panel()));

        // Object properties: the collapsed inspector area expands, or a window opens by preference.
        const auto inspectorPane = area->viewportLayout().paneIds(AreaRole::Inspector).front();
        area->setCollapsed(inspectorPane, true);
        area->flush();
        window.findChild<QAction*>("view.properties")->trigger();
        CHECK_FALSE(area->viewportLayout().collapsed(inspectorPane));
        CHECK_FALSE(area->floating(AreaRole::Inspector));
        ui::MainWindow::setInspectorInWindow(true);
        window.findChild<QAction*>("view.properties")->trigger();
        REQUIRE(area->floating(AreaRole::Inspector));
        CHECK(area->floating(AreaRole::Inspector)->isVisible());
        ui::MainWindow::setInspectorInWindow(false);

        // Workspaces keep their own layouts across switches.
        auto* bar = window.workspaceBar();
        bar->switchTo(2);
        area->flush();
        CHECK(area->viewportLayout().viewportIds().size() == 4);
        CHECK(area->findChildren<ui::CollapsedStrip*>().size() == 1);
        bar->switchTo(0);
        area->flush();
        CHECK(area->viewportLayout().viewportIds().size() == 5);  // The history pane is still a viewport.
        bar->addFromTemplate("Modeling");
        CHECK(bar->workspaces().workspaces.back().name == "Modeling 2");
        CHECK(bar->active() == 3);
        bar->rename(3, "Mine");
        window.close();
    }
    {
        ui::MainWindow window;
        window.show();
        auto* bar = window.workspaceBar();
        REQUIRE(bar->workspaces().workspaces.size() == 4);
        CHECK(bar->active() == 3);
        CHECK(bar->workspaces().workspaces[3].name == "Mine");
        CHECK(bar->workspaces().workspaces[3].base == "Modeling");
        CHECK(bar->workspaces().workspaces[0].layout.viewportIds().size() == 5);
        bar->remove(3);
        CHECK(bar->active() == 2);
        bar->resetActive();
        bar->switchTo(0);
        bar->resetActive();
        CHECK(window.viewportArea()->viewportLayout().viewportIds().size() == 4);
        bar->step(-1);
        CHECK(bar->active() == 2);
    }
    settings.remove("ui/workspaces");
}

TEST_CASE("editable outliner commits names, drops, visibility and persistent filtering") {
    app();
    using namespace editor_test;
    Document doc(parsed("world { id 1 classname worldspawn group { id 2 } group { id 3 } } entity { id 4 classname func_detail } entity { id 5 classname info_target targetname beacon } entity { id 6 classname info_target targetname first targetname second }"));
    const auto solid = id(doc, addBox(doc, {0,0,0}, {8,8,8}, worldHandle(doc)));
    std::vector<NodeId> groups, entities;
    for (const auto h : nodes(doc)) {
        if (std::holds_alternative<Handle<Group>>(h)) groups.push_back(id(doc,h));
        if (doc.get(h)->block->key() == "entity") entities.push_back(id(doc,h));
    }
    EditorSession session;
    session.attach(doc);
    struct Tree : apex::ui::OutlinerPanel {
        using OutlinerPanel::mimeData;
        using OutlinerPanel::dropMimeData;
    } tree;
    tree.setEditorSession(&session);
    tree.setDocument(&doc);
    tree.resize(420, 600);
    tree.show();
    app().processEvents();
    const auto row = [&](NodeId id) -> QTreeWidgetItem* {
        for (QTreeWidgetItemIterator it(&tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toULongLong() == id) return *it;
        return nullptr;
    };
    const auto rename = [&](NodeId id, const QString& name) {
        const auto matches = tree.model()->match(tree.model()->index(0,0), Qt::UserRole,
            QVariant::fromValue<qulonglong>(id), 1, Qt::MatchExactly | Qt::MatchRecursive);
        REQUIRE(matches.size() == 1);
        const auto index = matches.front();
        auto* delegate = tree.itemDelegate();
        auto* editor = delegate->createEditor(&tree, {}, index);
        REQUIRE(editor);
        delegate->setEditorData(editor, index);
        static_cast<QLineEdit*>(editor)->setText(name);
        delegate->setModelData(editor, tree.model(), index);
        delete editor;
        app().processEvents();
    };
    tree.setCurrentItem(row(entities[1]));
    tree.setFocus();
    QKeyEvent f2(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
    QApplication::sendEvent(&tree, &f2);
    auto* inlineEditor = tree.viewport()->findChild<QLineEdit*>();
    REQUIRE(inlineEditor);
    CHECK(inlineEditor->text() == "beacon");
    inlineEditor->setText("renamed_beacon");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(inlineEditor, &enter);
    app().processEvents(QEventLoop::AllEvents, 50);
    CHECK(row(entities[1])->text(0).contains("renamed_beacon"));
    const auto beforeFailedRename = *doc.write();
    const auto beforeFailedLabel = row(entities[2])->text(0);
    rename(entities[2], "rejected");
    CHECK(*doc.write() == beforeFailedRename);
    CHECK(row(entities[2])->text(0) == beforeFailedLabel);
    rename(entities[1], "");
    CHECK(doc.get(*doc.handle(entities[1]))->block->find("targetname") == nullptr);
    REQUIRE(session.perform("edit.undo"));
    const auto drop = [&](NodeId source, NodeId target) {
        std::unique_ptr<QMimeData> mime(tree.mimeData({row(source)}));
        return tree.dropMimeData(row(target), -1, mime.get(), Qt::MoveAction);
    };
    REQUIRE(drop(solid, groups[0]));
    CHECK(session.groups().parentGroup(solid) == groups[0]);
    CHECK(row(solid)->parent() == row(groups[0]));
    REQUIRE(drop(groups[0], groups[1]));
    const auto beforeInvalid = *doc.write();
    CHECK_FALSE(drop(groups[1], groups[0]));
    CHECK_FALSE(drop(entities[1], entities[0]));
    CHECK(*doc.write() == beforeInvalid);
    REQUIRE(drop(solid, entities[0]));
    CHECK(doc.get(*doc.handle(solid))->parent == entities[0]);
    session.selection().add(entities[1]);
    const auto selection = session.selection().toBlob();
    QStyleOptionViewItem checkOption;
    checkOption.initFrom(&tree);
    checkOption.rect = tree.visualItemRect(row(solid));
    checkOption.features = QStyleOptionViewItem::HasCheckIndicator;
    const auto checkPoint = tree.style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,
                                                        &checkOption, &tree).center();
    QMouseEvent press(QEvent::MouseButtonPress, checkPoint, checkPoint, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, checkPoint, checkPoint, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree.viewport(), &press);
    QApplication::sendEvent(tree.viewport(), &release);
    app().processEvents();
    CHECK(session.nodeHidden(solid));
    CHECK(session.selection().toBlob() == selection);
    CHECK(row(solid)->foreground(0) == tree.palette().brush(QPalette::Disabled, QPalette::Text));
    row(solid)->setCheckState(0, Qt::Checked);
    app().processEvents();
    CHECK_FALSE(session.nodeHidden(solid));
    auto* filter = tree.findChild<QLineEdit*>("outlinerFilter");
    REQUIRE(filter);
    CHECK(filter->mapTo(&tree, filter->rect().bottomLeft()).y() < tree.viewport()->geometry().top());
    filter->setText("RENAMED_BEACON");
    CHECK_FALSE(row(entities[1])->isHidden());
    CHECK_FALSE(row(entities[1])->parent()->isHidden());
    CHECK(row(solid)->isHidden());
    tree.refresh();
    CHECK(filter->text() == "RENAMED_BEACON");
    CHECK(row(solid)->isHidden());
    filter->clear();
    CHECK_FALSE(row(solid)->isHidden());
    row(groups[1])->setExpanded(false);
    tree.refresh();
    CHECK_FALSE(row(groups[1])->isExpanded());
    std::unique_ptr<QMimeData> mime(tree.mimeData({row(solid)}));
    Tree other;
    other.setEditorSession(&session);
    other.setDocument(&doc);
    const auto beforeForeignDrop = *doc.write();
    CHECK_FALSE(other.dropMimeData(other.topLevelItem(0), -1, mime.get(), Qt::MoveAction));
    CHECK(*doc.write() == beforeForeignDrop);
    CHECK_FALSE(mime->hasFormat("application/x-qabstractitemmodeldatalist"));
}

#include "assets/fgd.hpp"
#include "ui/inspector_panel.hpp"
#include <QCompleter>
#include <QPushButton>
#include <QFocusEvent>
#include <QFormLayout>

namespace {
const char* inspectorFgd = R"(@PointClass = test [
    text(string) : "Text" : "hello"
    count(integer) : "Count" : 3
    rate(float) : "Rate" : "1.5"
    mode(choices) : "Mode" : 1 = [ 1 : "One" 2 : "Two" ]
    spawnflags(flags) = [ 1 : "One" : 0 4 : "Four" : 0 ]
    color(color255) : "Colour" : "255 255 255"
    origin(origin) : "Origin" : "0 0 0"
    angles(angle) : "Angles" : "0 0 0"
    target(target_destination) : "Target"
    model(studio) : "Model"
    material(material) : "Material"
    noise(sound) : "Sound"
    locked(string) readonly : "Locked" : "keep"
]
@PointClass = second [ text(string) : "Text" ]
@SolidClass = brush []
)";
void putFgd(const QString& path) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(inspectorFgd);
}
void typeText(QLineEdit* edit, const QString& value) {
    REQUIRE(edit);
    edit->setFocus();
    edit->selectAll();
    QKeyEvent event(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, value);
    QApplication::sendEvent(edit, &event);
    app().processEvents();
}
std::map<std::string, std::string> inspectorValues(const document::Document& doc, document::NodeId id) {
    std::map<std::string, std::string> values;
    for (const auto& key : doc.get(*doc.handle(id))->block->children())
        if (key.kind() == kv1::NodeKind::Value) values[std::string(key.key())] = key.decodedValue().value();
    return values;
}
}

TEST_CASE("inspector builds FGD controls and coalesces one field focus") {
    app();
    QTemporaryDir dir;
    const auto path = dir.filePath("inspector.fgd");
    putFgd(path);
    auto table = assets::loadEntityClasses(std::filesystem::path(path.toStdString()));
    REQUIRE(table);
    document::Document doc(editor_test::parsed(R"(entity { id 1 classname test text first spawnflags 8 origin "1 2 3" unknown keep }
        entity { id 2 classname test text second spawnflags 9 origin "4 5 6" targetname destination })"));
    const auto first = doc.roots()[0], second = doc.roots()[1];
    editor::EditorSession session;
    session.attach(doc);
    session.setEntityClasses(*table);
    ui::InspectorPanel panel;
    panel.setDocument(&doc);
    panel.setEditorSession(&session);
    panel.resize(500, 900);
    panel.show();
    session.selectNode(first);
    app().processEvents();
    auto* edit = panel.findChild<QLineEdit*>("edit.text.0");
    REQUIRE(edit);
    CHECK(edit->text() == "first");
    CHECK_FALSE(panel.findChild<QLineEdit*>("edit.locked.0")->isEnabled());
    CHECK(panel.findChild<QLineEdit*>("edit.target.0")->completer());
    CHECK(panel.findChild<QLineEdit*>("edit.origin.2"));
    CHECK(panel.findChild<QLineEdit*>("edit.angles.2"));
    CHECK(panel.findChild<QComboBox*>("choice.mode"));
    int disabledBrowse = 0;
    for (auto* button : panel.findChildren<QPushButton*>())
        if (button->text().startsWith("Browse") && !button->isEnabled()) ++disabledBrowse;
    CHECK(disabledBrowse == 3);
    auto expected = inspectorValues(doc, first);
    const auto initial = doc.undoCount();
    typeText(edit, "changed");
    typeText(edit, "changed again");
    CHECK(panel.findChild<QLineEdit*>("edit.text.0") == edit);
    CHECK(edit->hasFocus());
    expected["text"] = "changed again";
    CHECK(inspectorValues(doc, first) == expected);
    CHECK(doc.undoCount() == initial + 1);
    edit->clearFocus();
    app().processEvents();
    edit = panel.findChild<QLineEdit*>("edit.text.0");
    typeText(edit, "next focus");
    CHECK(doc.undoCount() == initial + 2);
    edit->clearFocus();
    app().processEvents();
    auto* flag = panel.findChild<QCheckBox*>("flag.spawnflags.4");
    REQUIRE(flag);
    flag->setFocus();
    const auto flagHistory = doc.undoCount();
    flag->click();
    flag->click();
    flag->click();
    app().processEvents();
    CHECK(doc.undoCount() == flagHistory + 1);
    flag->clearFocus();
    app().processEvents();
    expected["text"] = "next focus";
    expected["spawnflags"] = "12";
    CHECK(inspectorValues(doc, first) == expected);
    REQUIRE(session.perform("edit.undo"));
    CHECK(inspectorValues(doc, first)["spawnflags"] == "8");
    session.selection().add(second);
    app().processEvents();
    edit = panel.findChild<QLineEdit*>("edit.text.0");
    REQUIRE(edit);
    CHECK(edit->text().isEmpty());
    CHECK(edit->placeholderText() == "(mixed)");
    const auto beforeMixed = doc.write().value();
    const auto beforeHistory = doc.undoCount();
    edit->setFocus();
    edit->clearFocus();
    app().processEvents();
    CHECK(doc.write().value() == beforeMixed);
    CHECK(doc.undoCount() == beforeHistory);
    flag = panel.findChild<QCheckBox*>("flag.spawnflags.4");
    REQUIRE(flag);
    flag->click();
    app().processEvents();
    CHECK(inspectorValues(doc, first)["spawnflags"] == "12");
    CHECK(inspectorValues(doc, second)["spawnflags"] == "13");
    auto* x = panel.findChild<QLineEdit*>("edit.origin.0");
    REQUIRE(x);
    CHECK(x->placeholderText() == "(mixed)");
    const auto vectorHistory = doc.undoCount();
    typeText(x, "10");
    typeText(x, "20");
    CHECK(doc.undoCount() == vectorHistory + 1);
    CHECK(inspectorValues(doc, first)["origin"] == "20 2 3");
    CHECK(inspectorValues(doc, second)["origin"] == "20 5 6");
    x->clearFocus();
    app().processEvents();
    auto beforeClass = inspectorValues(doc, first);
    auto* picker = panel.findChild<QComboBox*>("inspectorClass");
    REQUIRE(picker);
    picker->setCurrentIndex(picker->findText("second"));
    picker->activated(picker->currentIndex());
    app().processEvents();
    beforeClass["classname"] = "second";
    CHECK(inspectorValues(doc, first) == beforeClass);
    CHECK(inspectorValues(doc, second)["classname"] == "second");
    session.selectNode(first);
    app().processEvents();
    edit = panel.findChild<QLineEdit*>("edit.text.0");
    REQUIRE(edit);
    edit->setFocus();
    const auto beforeSelectionChange = doc.write().value();
    session.selectNode(second);
    typeText(edit, "stale input");
    CHECK(doc.write().value() == beforeSelectionChange);
    CHECK(panel.findChild<QLineEdit*>("edit.text.0")->text() == "second");
    panel.setEditorSession(nullptr);
    panel.setDocument(nullptr);
}

TEST_CASE("raw inspector preserves duplicate order and edits one occurrence") {
    app();
    document::Document doc(editor_test::parsed("entity { id 1 classname test extra first extra second tail last }"));
    editor::EditorSession session;
    session.attach(doc);
    session.selectNode(doc.roots().front());
    ui::InspectorPanel panel;
    panel.setDocument(&doc);
    panel.setEditorSession(&session);
    panel.show();
    auto* raw = panel.findChild<QCheckBox*>("inspectorRaw");
    REQUIRE(raw);
    raw->setChecked(true);
    app().processEvents();
    auto* first = panel.findChild<QLineEdit*>("edit.extra.0");
    auto* second = panel.findChild<QLineEdit*>("edit.extra.1");
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->text() == "first");
    CHECK(second->text() == "second");
    auto* form = qobject_cast<QFormLayout*>(first->parentWidget()->parentWidget()->layout());
    REQUIRE(form);
    int firstRow = -1, secondRow = -1;
    QFormLayout::ItemRole role;
    form->getWidgetPosition(first->parentWidget(), &firstRow, &role);
    form->getWidgetPosition(second->parentWidget(), &secondRow, &role);
    CHECK(firstRow < secondRow);
    typeText(second, "replacement");
    const auto& block = doc.get(*doc.handle(doc.roots().front()))->block;
    const auto keys = block->findAll("extra");
    REQUIRE(keys.size() == 2);
    CHECK(keys[0]->decodedValue().value() == "first");
    CHECK(keys[1]->decodedValue().value() == "replacement");
    panel.setDocument(nullptr);
    CHECK(panel.findChildren<QLineEdit*>().size() == 1); // Class picker's editor only.
}

TEST_CASE("entity toolbar follows session actions and loads point classes from profile") {
    app();
    QTemporaryDir dir;
    putFgd(dir.filePath("test.fgd"));
    QSettings settings;
    settings.setValue("editor/entityClass", "second");
    ui::MainWindow window;
    auto* picker = window.findChild<QComboBox*>("entityClassPicker");
    REQUIRE(picker);
    CHECK_FALSE(picker->isEnabled());
    assets::GameProfile profile;
    profile.name = "Inspector test";
    profile.fgds.push_back(std::filesystem::path(dir.filePath("test.fgd").toStdString()));
    window.setProfiles({profile});
    CHECK(picker->isEnabled());
    CHECK(picker->count() == 2);
    CHECK(picker->findText("brush") == -1);
    CHECK(picker->currentText() == "second");
    CHECK(window.editorSession().currentEntityClass() == "second");
    auto* entity = window.findChild<QAction*>("tool.entity");
    auto* select = window.findChild<QAction*>("tool.select");
    auto* toolbar = window.findChild<QToolBar*>("toolbarTransform");
    REQUIRE(entity);
    REQUIRE(select);
    REQUIRE(toolbar);
    CHECK(toolbar->actions().contains(entity));
    CHECK(toolbar->actions().contains(select));
    window.findChild<QAction*>("preset.Hammer")->trigger();
    CHECK(entity->shortcut() == QKeySequence("Shift+E"));
    CHECK(select->shortcut() == QKeySequence("Shift+S"));
    REQUIRE(window.editorSession().perform("tool.entity"));
    CHECK(entity->isChecked());
    CHECK_FALSE(select->isChecked());
    CHECK(qobject_cast<QToolButton*>(toolbar->widgetForAction(entity))->isChecked());
    select->trigger();
    CHECK(window.editorSession().activeTool() == editor::EditorSession::Tool::Select);
    CHECK(select->isChecked());
    auto* align = window.findChild<QCheckBox*>("entityAlignToSurface");
    REQUIRE(align);
    align->click();
    CHECK(window.editorSession().alignToSurface());
    picker->setCurrentIndex(picker->findText("test"));
    CHECK(window.editorSession().currentEntityClass() == "test");
    CHECK(settings.value("editor/entityClass").toString() == "test");
    window.setProfiles({});
    CHECK_FALSE(picker->isEnabled());
    CHECK(window.editorSession().entityClasses().classes.empty());
}

#include <QInputDialog>
#include <QTimer>

TEST_CASE("undo cap setting defaults clamps installs and applies live") {
    app();
    QSettings settings;
    const auto previous = settings.value("history/undoCapMiB");
    struct Restore {
        QVariant previous;
        ~Restore() {
            if (previous.isValid()) QSettings().setValue("history/undoCapMiB", previous);
            else QSettings().remove("history/undoCapMiB");
        }
    } restore{previous};
    for (const auto& [value, expected] : std::vector<std::pair<QVariant, int>>{
             {QVariant{}, 512}, {"invalid", 512}, {-3, 16}, {15, 16}, {8193, 8192}, {"128", 128}}) {
        if (value.isValid()) settings.setValue("history/undoCapMiB", value);
        else settings.remove("history/undoCapMiB");
        ui::MainWindow window;
        window.newFile();
        REQUIRE(window.document());
        CHECK(window.document()->undoCap() == static_cast<std::size_t>(expected) * 1024 * 1024);
        REQUIRE(window.openFile(QStringLiteral(APEX_FIXTURE_DIR) + "/vmf/everything_lf.vmf"));
        CHECK(window.document()->undoCap() == static_cast<std::size_t>(expected) * 1024 * 1024);
    }
    ui::MainWindow window;
    window.newFile();
    auto* action = window.findChild<QAction*>("actionUndoMemoryLimit");
    REQUIRE(action);
    QTimer::singleShot(0, &window, [&window] {
        auto* dialog = window.findChild<QInputDialog*>();
        REQUIRE(dialog);
        dialog->setIntValue(64);
        dialog->accept();
    });
    action->trigger();
    CHECK(settings.value("history/undoCapMiB").toInt() == 64);
    CHECK(window.document()->undoCap() == 64 * 1024 * 1024);
    auto* panel = window.findChild<ui::HistoryPanel*>();
    REQUIRE(panel);
    CHECK(panel->memoryLabel()->text().contains("/ 64 MiB"));
}

// getRange is public in Qt 6; keep the requested/result/range probe in the test subclass.
namespace {
class SplitterDragProbe : public ui::ViewportArea {
public:
    void dragTo(QSplitter* splitter, int target) {
        auto* handle = splitter->handle(1);
        const bool horizontal = splitter->orientation() == Qt::Horizontal;
        const auto position = [&] { return horizontal ? handle->x() : handle->y(); };
        const int initial = position();
        const QPointF local = handle->rect().center();
        const QPointF start = handle->mapToGlobal(local);
        QMouseEvent press(QEvent::MouseButtonPress, local, start, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(handle, &press);
        for (int step = 1; step <= 20; ++step) {
            const int requested = initial + qRound((target - 13 - initial) * (step / 20.0));
            const QPointF at = start + (horizontal ? QPointF(requested - initial, 0) : QPointF(0, requested - initial));
            QMouseEvent move(QEvent::MouseMove, handle->mapFromGlobal(at), at, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
            QCoreApplication::sendEvent(handle, &move);
            flush();
            int minimum = 0, maximum = 0;
            splitter->getRange(1, &minimum, &maximum);
            MESSAGE("drag step=" << step << " pointer=" << at.x() << "," << at.y()
                    << " requested=" << requested << " handle=" << position()
                    << " range=[" << minimum << "," << maximum << "]");
            CHECK(std::abs(position() - std::clamp(requested, minimum, maximum)) <= 1);
        }
        const QPointF end = start + (horizontal ? QPointF(target - initial, 0) : QPointF(0, target - initial));
        QMouseEvent release(QEvent::MouseButtonRelease, handle->mapFromGlobal(end), end, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(handle, &release);
        int minimum = 0, maximum = 0;
        splitter->getRange(1, &minimum, &maximum);
        MESSAGE("release pointer=" << end.x() << "," << end.y() << " requested=" << target
                << " handle=" << position() << " range=[" << minimum << "," << maximum << "]");
        CHECK(std::abs(position() - std::clamp(target, minimum, maximum)) <= 1);
    }
};
}

TEST_CASE("quad splitter reaches legal pointer targets and clamps only at its range") {
    ui::applyTheme(app(), ui::Theme::Dark);
    SplitterDragProbe area;
    area.resize(800, 600);
    area.show();
    area.flush();
    for (auto* splitter : area.findChildren<QSplitter*>()) {
        for (const int target : {20, 150, 650, 350, 1000}) area.dragTo(splitter, target);
    }
}

TEST_CASE("splitter maps local motion from changing native receivers despite bogus global positions") {
    ui::applyTheme(app(), ui::Theme::Dark);
    ui::ViewportArea area;
    area.resize(800, 600);
    area.move(120, 90);
    area.show();
    area.flush();
    REQUIRE(area.windowHandle());
    // Native pane stand-ins: no Vulkan or compositor needed. Application filters must
    // receive their moves before pane mouse handling can consume them.
    class Receiver : public QWindow {
    public:
        using QWindow::QWindow;
        int moves = 0;
        int releases = 0;
        bool event(QEvent* event) override {
            if (event->type() == QEvent::MouseMove) ++moves;
            if (event->type() == QEvent::MouseButtonRelease) ++releases;
            return QWindow::event(event);
        }
    };
    Receiver first(area.windowHandle()), second(area.windowHandle());
    first.setGeometry(43, 67, 250, 180);
    second.setGeometry(350, 280, 270, 190);
    QWidget widget(&area);
    widget.move(61, 89);
    const auto splitters = area.findChildren<QSplitter*>();
    REQUIRE(splitters.size() == 3);
    for (auto* splitter : splitters) {
        auto* handle = splitter->handle(1);
        const bool horizontal = splitter->orientation() == Qt::Horizontal;
        const auto position = [&] { return horizontal ? handle->x() : handle->y(); };
        const int initial = position();
        const QPointF local = handle->rect().center();
        const QPointF start = handle->mapToGlobal(local);
        const auto offset = [&](int delta) { return horizontal ? QPointF(delta, 0) : QPointF(0, delta); };
        // Even the press's reported global origin is unreliable.
        QMouseEvent press(QEvent::MouseButtonPress, local, QPointF(-9000, 7000),
                          Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(handle, &press);
        REQUIRE(area.handleDragging());
        for (int step = 1; step <= 6; ++step) {
            // Resize/reposition a receiver between deliveries, as splitter layout does.
            first.setGeometry(43 + step * 11, 67 + step * 7, 250 + step, 180 + step);
            QObject* receiver = step % 3 == 0 ? static_cast<QObject*>(&widget) :
                                step % 2 == 0 ? static_cast<QObject*>(&second) : &first;
            const QPointF at = start + offset(step * 9);
            const QPointF receiverLocal = receiver == &widget ? widget.mapFromGlobal(at) :
                                         static_cast<QWindow*>(receiver)->mapFromGlobal(at);
            QMouseEvent move(QEvent::MouseMove, receiverLocal, QPointF(8000 - step * 200, -6000),
                             Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
            QCoreApplication::sendEvent(receiver, &move);
        }
        CHECK(position() == initial); // Six deliveries only replaced one pending position.
        area.flush();
        int minimum = 0, maximum = 0;
        splitter->getRange(1, &minimum, &maximum);
        CHECK(position() == std::clamp(initial + 6 * 9, minimum, maximum));
        const QPointF end = start + offset(67); // Release includes a final 13 px.
        second.setPosition({205, 143});
        QMouseEvent release(QEvent::MouseButtonRelease, second.mapFromGlobal(end), QPointF(-4000, 9000),
                            Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(&second, &release);
        CHECK_FALSE(area.handleDragging());
        splitter->getRange(1, &minimum, &maximum);
        CHECK(position() == std::clamp(initial + 67, minimum, maximum));
    }
    CHECK(first.moves == 0);
    CHECK(second.moves == 0);
    CHECK(second.releases == 3);
    QMouseEvent normalMove(QEvent::MouseMove, QPointF(20, 20), QPointF(),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&first, &normalMove);
    CHECK(first.moves == 1); // The filter is removed after the drag.
}

TEST_CASE("splitter flood coalesces to one apply per explicit tick") {
    ui::applyTheme(app(), ui::Theme::Dark);
    ui::ViewportArea area;
    area.resize(800, 600);
    area.show();
    area.flush();
    auto* splitter = area.findChild<QSplitter*>();
    REQUIRE(splitter);
    auto* handle = splitter->handle(1);
    const bool horizontal = splitter->orientation() == Qt::Horizontal;
    const auto position = [&] { return horizontal ? handle->x() : handle->y(); };
    const int initial = position();
    const QPointF local = handle->rect().center();
    const QPointF start = handle->mapToGlobal(local);
    QMouseEvent press(QEvent::MouseButtonPress, local, start,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &press);
    REQUIRE(area.handleDragging());

    QElapsedTimer flood;
    flood.start();
    for (int step = 1; step <= 500; ++step) {
        const int delta = step % 151;
        const QPointF at = start + (horizontal ? QPointF(delta, 0) : QPointF(0, delta));
        QMouseEvent move(QEvent::MouseMove, handle->mapFromGlobal(at), at,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &move);
    }
    const double deliveryMs = static_cast<double>(flood.nsecsElapsed()) / 1.0e6;
    CHECK(position() == initial);
    QElapsedTimer apply;
    apply.start();
    area.flush();
    const double applyMs = static_cast<double>(apply.nsecsElapsed()) / 1.0e6;
    const int finalDelta = 500 % 151;
    int minimum = 0, maximum = 0;
    splitter->getRange(1, &minimum, &maximum);
    CHECK(position() == std::clamp(initial + finalDelta, minimum, maximum));
    MESSAGE("500 splitter moves: delivery_ms=" << deliveryMs << " one_apply_tick_ms=" << applyMs);

    const QPointF end = start + (horizontal ? QPointF(73, 0) : QPointF(0, 73));
    QMouseEvent release(QEvent::MouseButtonRelease, handle->mapFromGlobal(end), end,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &release);
    CHECK_FALSE(area.handleDragging());
    splitter->getRange(1, &minimum, &maximum);
    CHECK(position() == std::clamp(initial + 73, minimum, maximum));
}

TEST_CASE("splitter cancels on lost button escape deactivation hide and rebuild") {
    ui::applyTheme(app(), ui::Theme::Dark);
    ui::ViewportArea area;
    area.resize(800, 600);
    area.show();
    area.flush();
    const auto begin = [&] {
        auto* handle = area.findChild<QSplitter*>()->handle(1);
        const QPointF local = handle->rect().center();
        QMouseEvent press(QEvent::MouseButtonPress, local, handle->mapToGlobal(local),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &press);
        REQUIRE(area.handleDragging());
    };
    begin();
    QMouseEvent lost(QEvent::MouseMove, QPointF(30, 40), QPointF(), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&area, &lost);
    CHECK_FALSE(area.handleDragging());
    begin();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&area, &escape);
    CHECK_FALSE(area.handleDragging());
    begin();
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QCoreApplication::sendEvent(qApp, &deactivate);
    CHECK_FALSE(area.handleDragging());
    begin();
    area.hide();
    CHECK_FALSE(area.handleDragging());
    area.show();
    begin();
    area.resetLayout();
    CHECK_FALSE(area.handleDragging());
}

TEST_CASE("scripted 200-step splitter drag preserves frames between resize intervals") {
    ui::applyTheme(app(), ui::Theme::Dark);
    ui::ViewportArea area;
    area.resize(800, 600);
    area.show();
    QCoreApplication::processEvents();
    area.flush();
    auto* splitter = area.findChild<QSplitter*>();
    REQUIRE(splitter);
    auto* handle = splitter->handle(1);
    const bool horizontal = splitter->orientation() == Qt::Horizontal;
    const QPointF local = handle->rect().center();
    const QPointF start = handle->mapToGlobal(local);
    std::map<viewport::NodeId, std::uint32_t> initial;
    for (auto id : area.viewportLayout().paneIds()) {
        if (auto* window = area.frame(id)->window()) initial[id] = window->recreateCount();
    }
    QMouseEvent press(QEvent::MouseButtonPress, local, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &press);
    REQUIRE(area.handleDragging());
    QElapsedTimer elapsed;
    elapsed.start();
    for (int step = 1; step <= 200; ++step) {
        QElapsedTimer frame;
        frame.start();
        const QPointF at = start + (horizontal ? QPointF(step, 0) : QPointF(0, step));
        QMouseEvent move(QEvent::MouseMove, handle->mapFromGlobal(at), at, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &move);
        QCoreApplication::processEvents();
        area.flush();
        MESSAGE("splitter step=" << step << " elapsed_ms=" << elapsed.elapsed()
                << " step_ms=" << static_cast<double>(frame.nsecsElapsed()) / 1.0e6);
        for (const auto& [id, count] : initial) {
            const auto* window = area.frame(id)->window();
            CHECK_FALSE(window->failed());
            CHECK(window->recreateCount() - count <= static_cast<std::uint32_t>(elapsed.elapsed() / 16 + 1));
        }
    }
    std::map<viewport::NodeId, std::uint32_t> beforeRelease;
    for (const auto& [id, count] : initial) beforeRelease[id] = area.frame(id)->window()->recreateCount();
    const QPointF end = start + (horizontal ? QPointF(200, 0) : QPointF(0, 200));
    QMouseEvent release(QEvent::MouseButtonRelease, handle->mapFromGlobal(end), end, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &release);
    CHECK_FALSE(area.handleDragging());
    area.flush();
    for (const auto& [id, count] : beforeRelease) {
        const auto* window = area.frame(id)->window();
        CHECK_FALSE(window->failed());
        CHECK(window->recreateCount() == count + 1);
    }
    if (initial.empty()) MESSAGE("No native GPU panes on this platform; run with QT_QPA_PLATFORM=xcb for frame measurements");
}

TEST_CASE("block tool draws through the pane's TransformInput path despite CancelPointer after consumed events") {
    app();
    TransformScene scene;
    auto& session = scene.session();
    REQUIRE(session.perform("tool.block"));
    session.selection().clear();
    const auto camera = cameraLooking({37, -60, 260}, {37, 37, 69});
    ui::TransformInput input;
    input.setSession(&session);
    // PaneWindow::handleMouse sends CancelPointer whenever the active tool consumes an event.
    const auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        auto event = mouseEvent(type, at, button, buttons);
        const bool consumed = input.mouse(event, camera, kPane);
        if (consumed) input.cancelPointer();
        return consumed;
    };
    // Pixel whose ray meets the plane z = target.z closest to target.
    const auto pixelFor = [&](DVec3 target) {
        QPointF best;
        double bestDistance = 1e18;
        for (double y = 0; y < kPane.height(); y += 1)
            for (double x = 0; x < kPane.width(); x += 1) {
                const auto ray = rayAtPixel(camera, {x, y});
                if (std::abs(ray.dir.z) < 1e-9) continue;
                const double t = (target.z - ray.origin.z) / ray.dir.z;
                if (t <= 0) continue;
                const double d = glm::length(ray.at(t) - target);
                if (d < bestDistance) { bestDistance = d; best = {x, y}; }
            }
        return best;
    };
    const QPointF a = pixelFor({20, 20, 69}), b = pixelFor({55, 50, 69}), up = b - QPointF(0, 80);
    const auto before = scene.undo();
    send(QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton);
    CHECK(send(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton));
    CHECK(send(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton));
    CHECK(send(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton));
    CHECK(send(QEvent::MouseMove, up, Qt::NoButton, Qt::NoButton));
    REQUIRE(session.preview().ghosts().size() == 1);
    CHECK(scene.undo() == before);
    CHECK(send(QEvent::MouseButtonPress, up, Qt::LeftButton, Qt::LeftButton));
    send(QEvent::MouseButtonRelease, up, Qt::LeftButton, Qt::NoButton);
    CHECK(scene.undo() == before + 1);
    CHECK(session.preview().empty());
    REQUIRE(session.selection().ordered().size() == 1);
    const auto handle = scene.window.document()->handle(std::get<document::NodeId>(session.selection().ordered().front()));
    REQUIRE(handle);
    const auto solid = geometry::solidGeometry(*scene.window.document()->get(*handle)->block);
    REQUIRE(solid);
    CHECK(solid->poly.bounds().min.z == doctest::Approx(69));  // Drawn on the box's top face.
    CHECK(solid->poly.bounds().max.z > 69);

    // Esc through the key path drops a gesture without touching the document.
    send(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    input.key(esc, camera, kPane, true);
    CHECK(session.preview().empty());
    CHECK(scene.undo() == before + 1);
}

// Destroying a native pane hides its window, which cancels tool input and can notify session
// observers mid-teardown; updateModeBadges then wrote to a frame being destroyed (segfault switching
// layouts with the block tool active). Offscreen panes never hide natively, so notify from a child.
TEST_CASE("viewport area survives session notifications while it destroys panes") {
    app();
    ui::MainWindow window;
    window.show();
    auto* area = window.viewportArea();
    area->flush();
    auto& session = window.editorSession();
    REQUIRE(session.perform("tool.block"));
    const auto frames = area->findChildren<ui::PaneFrame*>();
    REQUIRE(frames.size() == 4);
    int notified = 0;
    for (auto* frame : frames) {
        auto* spy = new QObject(frame);  // Created last, so destroyed after the frame's labels.
        QObject::connect(spy, &QObject::destroyed, [&session, &notified] {
            session.setAlignToSurface(!session.alignToSurface());  // Any change notifies observers.
            ++notified;
        });
    }
    auto maximized = viewport::ViewportLayout(area->viewportLayout());
    maximized.toggleMaximize(maximized.paneIds().front());
    area->setLayout(std::move(maximized));
    area->flush();
    CHECK(notified == 4);
    area->setLayout(viewport::ViewportLayout(area->viewportLayout()));  // Rebuilt frames take badge updates.
    session.setAlignToSurface(!session.alignToSurface());
}
