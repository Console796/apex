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
    for (const char* name : {"statusGizmoTranslate", "statusGizmoRotate", "statusGizmoScale", "statusSnapMode",
                             "statusTextureLock"})
        CHECK(scene.window.findChild<QToolButton*>(name));
    CHECK(scene.window.findChild<QLabel*>("statusGrid"));

    // Gizmo modes: exclusive, mirrored on toolbar action and status button.
    CHECK(scene.action("gizmo.mode.translate")->isChecked());
    scene.action("gizmo.mode.rotate")->trigger();
    CHECK(session.gizmoMode() == editor::GizmoMode::Rotate);
    CHECK(scene.action("gizmo.mode.rotate")->isChecked());
    CHECK_FALSE(scene.action("gizmo.mode.translate")->isChecked());
    CHECK(scene.window.findChild<QToolButton*>("statusGizmoRotate")->isChecked());
    scene.window.findChild<QToolButton*>("statusGizmoScale")->click();
    CHECK(session.gizmoMode() == editor::GizmoMode::Scale);
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
