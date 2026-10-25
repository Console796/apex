#include "test_app.hpp"
#include "../assets/fixture.hpp"
#include "../assets/texture_fixture.hpp"
#include "../editor/fixtures.hpp"

#include <doctest/doctest.h>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QToolButton>

#include "document/document.hpp"
#include "document/vmf.hpp"
#include "editor/editor_session.hpp"
#include "geometry/solid_block.hpp"
#include "ui/face_properties_panel.hpp"
#include "ui/game_content.hpp"
#include "ui/main_window.hpp"
#include "ui/viewport_area.hpp"
#include "ui/uv_view.hpp"
#include "viewport/layout.hpp"

using namespace apex;
namespace {
QApplication& app() { return apex::ui_test::app(); }
assets::GameProfile profile(GameTree& tree) {
    tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("Game/materials/brick.vmt", "LightmappedGeneric { $basetexture white }");
    auto image = whiteVtf();
    REQUIRE(image.bake(tree.root / "Game/materials/white.vtf"));
    assets::GameProfile result;
    result.name = "Face properties UI";
    result.gameDirectory = tree.root / "Game";
    return result;
}
void waitForIndex(ui::GameContent& content) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!content.snapshot() && std::chrono::steady_clock::now() < deadline) {
        app().processEvents();
        QThread::msleep(1);
    }
    REQUIRE(content.snapshot());
}
struct Scene {
    document::Document doc{editor_test::parsed()};
    editor::EditorSession session;
    document::NodeId box = 0;
    Scene() {
        auto tx = doc.begin("face panel box");
        const auto handle = tx.create(geometry::solidBlock(
            geometry_test::build(geometry_test::boxSides()), {"apex/panel"}));
        box = doc.get(handle)->id;
        tx.commit();
        session.attach(doc);
        session.selection().setMode(editor::SelectionMode::Faces);
    }
    document::SideId side(DVec3 normal) const {
        auto geometry = geometry::solidGeometry(*doc.get(*doc.handle(box))->block);
        REQUIRE(geometry);
        for (const auto& face : geometry->poly.faces)
            if (glm::length(face.plane.normal - normal) < 1e-6) return static_cast<int>(face.sideId);
        return -1;
    }
};
}

TEST_CASE("Face Properties area role is listed, named and round trips layouts") {
    app();
    using viewport::AreaRole;
    auto layout = viewport::ViewportLayout::quad();
    const auto pane = layout.paneIds().front();
    REQUIRE(layout.setRole(pane, AreaRole::FaceProperties));
    const auto encoded = json::write(layout.toJson());
    CHECK(encoded.find("face_properties") != std::string::npos);
    const auto decoded = json::parse(encoded);
    REQUIRE(decoded);
    auto restored = viewport::ViewportLayout::fromJson(*decoded);
    REQUIRE(restored);
    CHECK(restored->role(pane) == AreaRole::FaceProperties);
    CHECK(viewport::roleFromName("face_properties") == AreaRole::FaceProperties);

    auto old = viewport::ViewportLayout::quad();
    auto oldDecoded = viewport::ViewportLayout::fromJson(old.toJson());
    REQUIRE(oldDecoded);
    CHECK(oldDecoded->paneIds(AreaRole::FaceProperties).empty());
    CHECK(viewport::nameOf(AreaRole::FaceProperties) == std::string_view("face_properties"));
    ui::MainWindow window;
    auto* roleButton = window.viewportArea()->findChild<QToolButton*>("AreaRoleButton");
    REQUIRE(roleButton);
    bool found = false;
    for (auto* action : roleButton->menu()->actions())
        found |= action->data().toString() == "face_properties" && action->text() == "Face Properties";
    CHECK(found);
    CHECK(window.viewportArea()->viewportLayout().paneIds(AreaRole::FaceProperties).empty());
}

TEST_CASE("Face Properties fields show mixed values, coalesce focused typing and preserve other bits") {
    app();
    Scene scene;
    const auto top = scene.side({0, 0, 1});
    const auto bottom = scene.side({0, 0, -1});
    scene.session.selection().select(top);
    REQUIRE(scene.session.setFaceField(editor::FaceField::ShiftU, 12.0));
    scene.session.selection().select(bottom);
    REQUIRE(scene.session.setFaceField(editor::FaceField::ShiftU, 24.0));
    scene.session.selection().select(top);
    REQUIRE(scene.session.setFaceField(editor::FaceField::SmoothingGroups, std::uint32_t{3}));
    scene.session.selection().select(bottom);
    REQUIRE(scene.session.setFaceField(editor::FaceField::SmoothingGroups, std::uint32_t{0}));
    scene.session.selection().select(top);
    scene.session.selection().add(bottom);

    ui::FacePropertiesPanel panel;
    panel.setDocument(&scene.doc);
    panel.setEditorSession(&scene.session);
    panel.show();
    app().processEvents();
    auto* shift = panel.findChild<QLineEdit*>("faceShiftU");
    auto* scale = panel.findChild<QLineEdit*>("faceScaleU");
    auto* smoothing = panel.findChild<QToolButton*>("smoothingGroup.1");
    REQUIRE(shift); REQUIRE(scale); REQUIRE(smoothing);
    CHECK(shift->text().isEmpty());
    CHECK(shift->placeholderText().contains("Mixed"));
    CHECK(smoothing->property("mixed").toBool());
    CHECK(panel.findChild<QWidget*>("facePropertiesContent")->isEnabled());

    const auto topShift = editor::faceSnapshot(scene.doc, top)->face.uv.shiftU;
    const auto bottomShift = editor::faceSnapshot(scene.doc, bottom)->face.uv.shiftU;
    scale->setFocus();
    app().processEvents();
    const auto beforeTyped = scene.doc.undoCount();
    scale->setText("0.5");
    scale->textEdited("0.5");
    scale->setText("0.75");
    scale->textEdited("0.75");
    CHECK(scene.doc.undoCount() == beforeTyped + 1);
    CHECK(editor::faceSnapshot(scene.doc, top)->face.uv.shiftU == topShift);
    CHECK(editor::faceSnapshot(scene.doc, bottom)->face.uv.shiftU == bottomShift);
    CHECK(editor::faceSnapshot(scene.doc, top)->face.uv.scaleU == doctest::Approx(0.75));
    scale->clearFocus();
    panel.refresh();

    scene.session.selection().select(top);
    panel.refresh();
    shift->setFocus();
    app().processEvents();
    const auto beforeShift = scene.doc.undoCount();
    shift->setText("30"); shift->textEdited("30");
    shift->setText("36"); shift->textEdited("36");
    CHECK(scene.doc.undoCount() == beforeShift + 1);
    CHECK(editor::faceSnapshot(scene.doc, top)->face.uv.shiftU == doctest::Approx(36));

    shift->clearFocus(); panel.refresh();
    auto* fit = panel.findChild<QPushButton*>("faceJustify.fit");
    REQUIRE(fit);
    const auto beforeFit = editor::faceSnapshot(scene.doc, top)->face.uv.scaleU;
    fit->click();
    CHECK(editor::faceSnapshot(scene.doc, top)->face.uv.scaleU != beforeFit);
    auto* topLeft = panel.findChild<QToolButton*>("faceAlign.tl");
    REQUIRE(topLeft);
    topLeft->click();
    CHECK(topLeft->isChecked());
    const auto alignedFace = editor::faceSnapshot(scene.doc, top).value();
    const auto [textureWidth, textureHeight] = scene.session.faceTextureSize(alignedFace.face.material);
    const auto position = geometry::alignmentOf(alignedFace.face.uv, alignedFace.points, textureWidth, textureHeight);
    CHECK(position.horizontal == geometry::HorizontalAlignment::Left);
    CHECK(position.vertical == geometry::VerticalAlignment::Top);
    auto* browse = panel.findChild<QPushButton*>("faceMaterialBrowse");
    REQUIRE(browse);
    std::function<void(QString)> choose;
    QString browseWhat;
    panel.setMaterialBrowser([&](QString, QString what, std::function<void(QString)> callback) {
        browseWhat = std::move(what);
        choose = std::move(callback);
    });
    CHECK(browse->isEnabled());
    browse->click(); REQUIRE(choose); CHECK(browseWhat == "the selected faces"); choose("apex/browsed");
    CHECK(editor::faceSnapshot(scene.doc, top)->face.material == "apex/browsed");

    // Typing a material writes nothing until Return, then one edit.
    auto* material = panel.findChild<QLineEdit*>("faceMaterial");
    REQUIRE(material);
    panel.refresh();
    material->setFocus();
    app().processEvents();
    const auto beforeMaterial = scene.doc.undoCount();
    material->selectAll();
    material->insert("apex/typed");
    CHECK(editor::faceSnapshot(scene.doc, top)->face.material == "apex/browsed");
    CHECK(scene.doc.undoCount() == beforeMaterial);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(material, &press);
    CHECK(editor::faceSnapshot(scene.doc, top)->face.material == "apex/typed");
    CHECK(scene.doc.undoCount() == beforeMaterial + 1);
    material->clearFocus();

    scene.session.selection().select(top);
    REQUIRE(scene.session.setFaceField(editor::FaceField::SmoothingGroups, std::uint32_t{2}));
    scene.session.selection().select(bottom);
    REQUIRE(scene.session.setFaceField(editor::FaceField::SmoothingGroups, std::uint32_t{0}));
    scene.session.selection().select(top);
    scene.session.selection().add(bottom);
    panel.refresh();
    auto* smoothingSummary = panel.findChild<QToolButton*>("faceSmoothingSummary");
    auto* smoothingGrid = panel.findChild<QWidget*>("faceSmoothingGroups");
    REQUIRE(smoothingSummary); REQUIRE(smoothingGrid);
    CHECK_FALSE(smoothingGrid->isVisible());
    smoothingSummary->click();
    CHECK(smoothingGrid->isVisible());
    smoothing->click();
    CHECK(editor::faceSnapshot(scene.doc, top)->face.smoothingGroups == 3);
    CHECK(editor::faceSnapshot(scene.doc, bottom)->face.smoothingGroups == 1);

    scene.session.selection().setMode(editor::SelectionMode::Solids);
    app().processEvents();
    CHECK_FALSE(panel.findChild<QWidget*>("facePropertiesContent")->isEnabled());
    CHECK(panel.findChild<QLabel*>("facePropertiesHint")->text().contains("Select faces"));
}

TEST_CASE("Face Properties buttons align sloped faces and report session errors") {
    app();
    Scene scene;
    auto tx = scene.doc.begin("add sloped test solid");
    const auto wedge = tx.create(geometry::solidBlock(geometry_test::build(geometry_test::wedgeSides()), {"apex/wedge"}));
    tx.commit();
    const auto wedgeId = scene.doc.get(wedge)->id;
    auto geometry = geometry::solidGeometry(*scene.doc.get(*scene.doc.handle(wedgeId))->block);
    REQUIRE(geometry);
    std::optional<document::SideId> sloped;
    for (const auto& face : geometry->poly.faces)
        if (std::abs(face.plane.normal.x) > .6 && std::abs(face.plane.normal.y) > .6)
            sloped = static_cast<int>(face.sideId);
    REQUIRE(sloped);
    scene.session.selection().select(*sloped);
    ui::FacePropertiesPanel panel;
    panel.setDocument(&scene.doc); panel.setEditorSession(&scene.session); panel.show(); app().processEvents();
    // New solids start face-aligned, so world alignment changes the sloped face and face alignment
    // brings the original axes back.
    auto* world = panel.findChild<QPushButton*>("faceAlignWorld");
    auto* align = panel.findChild<QPushButton*>("faceAlignFace");
    REQUIRE(world); REQUIRE(align);
    const auto before = editor::faceSnapshot(scene.doc, *sloped)->face.uv.uaxis;
    world->click();
    const auto worldAxis = editor::faceSnapshot(scene.doc, *sloped)->face.uv.uaxis;
    CHECK(glm::length(worldAxis - before) > 0.1);
    CHECK(geometry::deriveRotation(editor::faceSnapshot(scene.doc, *sloped)->face.uv,
                                   editor::faceSnapshot(scene.doc, *sloped)->face.plane.normal) == 0);
    align->click();
    CHECK(glm::length(editor::faceSnapshot(scene.doc, *sloped)->face.uv.uaxis - before) < 1e-5);
}

TEST_CASE("GameContent face texture dimensions read VTF headers and clear cache on reload") {
    app();
    GameTree tree;
    const auto game = profile(tree);
    ui::MainWindow window;
    window.setProfiles({game});
    waitForIndex(window.gameContent());
    auto& session = window.editorSession();
    CHECK(session.faceTextureSize("brick") == std::pair{8, 8});
    CHECK(session.faceTextureSize("unknown/material") == std::pair{256, 256});

    std::vector<std::byte> pixels(16 * 32 * 4, std::byte{255});
    vtfpp::VTF::CreationOptions options;
    options.computeThumbnail = false; options.computeReflectivity = false;
    auto larger = vtfpp::VTF::create(pixels, vtfpp::ImageFormat::RGBA8888, 16, 32, options);
    REQUIRE(larger.bake(tree.root / "Game/materials/white.vtf"));
    window.gameContent().reload(&game);
    waitForIndex(window.gameContent());
    CHECK(session.faceTextureSize("brick") == std::pair{16, 32});
}

TEST_CASE("Tool actions show in the toolbar and activate from buttons and Hammer shortcuts") {
    app();
    ui::MainWindow window;
    auto* apply = window.findChild<QAction*>("tool.apply");
    auto* measure = window.findChild<QAction*>("tool.measure");
    REQUIRE(apply); REQUIRE(measure);
    CHECK(apply->shortcut().toString() == "Shift+A");
    CHECK(measure->shortcut().toString() == "Shift+M");
    apply->trigger();
    CHECK(window.editorSession().activeTool() == "tool.apply");
    CHECK(window.editorSession().selection().mode() == editor::SelectionMode::Faces);
    measure->trigger();
    CHECK(window.editorSession().activeTool() == "tool.measure");

    editor::InputEvent shortcut;
    shortcut.kind = editor::InputKind::KeyDown;
    shortcut.chord = editor::KeyChord::parse("Shift+A").value();
    REQUIRE(window.editorSession().input(shortcut, {}));
    CHECK(window.editorSession().activeTool() == "tool.apply");
    shortcut.chord = editor::KeyChord::parse("Shift+M").value();
    REQUIRE(window.editorSession().input(shortcut, {}));
    CHECK(window.editorSession().activeTool() == "tool.measure");
}

TEST_CASE("Face edit activation expands Face Properties and the extrapolation combo cycles with the session") {
    app();
    QSettings settings;
    const auto oldWindow = settings.value("ui/facePropertiesWindow");
    const auto oldMode = settings.value("editor/faceExtrapolation");
    ui::MainWindow::setFacePropertiesInWindow(false);
    ui::MainWindow window;
    auto* area = window.viewportArea();
    const auto pane = area->viewportLayout().paneIds().front();
    area->setRole(pane, viewport::AreaRole::FaceProperties);
    app().processEvents();
    area->setCollapsed(pane, true);
    auto* action = window.findChild<QAction*>("tool.apply");
    REQUIRE(action);
    action->trigger();
    app().processEvents();
    CHECK_FALSE(area->viewportLayout().collapsed(pane));
    REQUIRE(area->panel(pane));
    auto* panel = qobject_cast<ui::FacePropertiesPanel*>(area->panel(pane));
    REQUIRE(panel);
    auto* mode = panel->findChild<QComboBox*>("faceExtrapolation");
    REQUIRE(mode);
    mode->setCurrentIndex(mode->findData(static_cast<int>(editor::FaceExtrapolation::Projected)));
    CHECK(window.editorSession().faceExtrapolation() == editor::FaceExtrapolation::Projected);
    REQUIRE(window.editorSession().perform("face.extrapolation.cycle"));
    app().processEvents();
    CHECK(window.editorSession().faceExtrapolation() == editor::FaceExtrapolation::Match);
    CHECK(mode->currentData().toInt() == static_cast<int>(editor::FaceExtrapolation::Match));
    if (oldWindow.isValid()) QSettings().setValue("ui/facePropertiesWindow", oldWindow);
    else QSettings().remove("ui/facePropertiesWindow");
    if (oldMode.isValid()) QSettings().setValue("editor/faceExtrapolation", oldMode);
    else QSettings().remove("editor/faceExtrapolation");
}

TEST_CASE("Face Properties reads decimals in any locale and drops focused text when the selection changes") {
    app();
    Scene scene;
    const auto top = scene.side({0, 0, 1});
    const auto bottom = scene.side({0, 0, -1});
    scene.session.selection().select(bottom);
    REQUIRE(scene.session.setFaceField(editor::FaceField::ShiftU, 24.0));
    scene.session.selection().select(top);
    ui::FacePropertiesPanel panel;
    panel.setDocument(&scene.doc);
    panel.setEditorSession(&scene.session);
    panel.show();
    app().processEvents();
    auto* shift = panel.findChild<QLineEdit*>("faceShiftU");
    REQUIRE(shift);

    // A German UI locale must not turn "1.5" into 0, nor accept "1,5" as something else.
    const QLocale previous;
    QLocale::setDefault(QLocale(QLocale::German));
    shift->setFocus();
    app().processEvents();
    shift->setText("1.5");
    emit shift->textEdited("1.5");
    CHECK(editor::faceSnapshot(scene.doc, top)->face.uv.shiftU == doctest::Approx(1.5));
    shift->setText("2,5");
    CHECK_FALSE(shift->hasAcceptableInput());
    QLocale::setDefault(previous);

    // Selecting other faces while the field has focus shows their values, and the next keystroke
    // edits only them, as a new undo step.
    shift->setText("1.5");
    scene.session.selection().select(bottom);
    panel.refresh();
    CHECK(shift->text() == "24");
    const auto undo = scene.doc.undoCount();
    shift->setText("30");
    emit shift->textEdited("30");
    CHECK(scene.doc.undoCount() == undo + 1);
    CHECK(editor::faceSnapshot(scene.doc, bottom)->face.uv.shiftU == doctest::Approx(30));
    CHECK(editor::faceSnapshot(scene.doc, top)->face.uv.shiftU == doctest::Approx(1.5));
    shift->clearFocus();
}

TEST_CASE("Face Properties in a small area keeps the area header on screen") {
    app();
    using viewport::AreaRole;
    ui::MainWindow window;
    window.resize(1280, 720);
    window.show();
    auto* area = window.viewportArea();
    for (const auto role : {AreaRole::Outliner, AreaRole::Inspector, AreaRole::History}) {
        const auto panes = area->viewportLayout().paneIds(role);
        if (panes.empty()) continue;
        area->setRole(panes.front(), AreaRole::FaceProperties);
        area->flush();
        app().processEvents();
        auto* panel = area->findChild<ui::FacePropertiesPanel*>();
        REQUIRE(panel);
        CHECK(panel->minimumSizeHint().width() <= 200);
        CHECK(panel->minimumSizeHint().height() <= 120);
        auto* hint = panel->findChild<QLabel*>("facePropertiesHint");
        REQUIRE(hint);
        CHECK(hint->height() <= hint->fontMetrics().height() + 4);
        QToolButton* roleButton = nullptr;
        for (auto* b : area->findChildren<QToolButton*>("AreaRoleButton"))
            if (b->text().startsWith("Face Properties")) roleButton = b;
        REQUIRE(roleButton);
        const QRect inWindow(roleButton->mapTo(&window, QPoint(0, 0)), roleButton->size());
        CHECK(roleButton->isVisible());
        CHECK(window.rect().contains(inWindow));
        area->setRole(area->viewportLayout().paneIds(AreaRole::FaceProperties).front(), role);
        area->flush();
    }
    const QSize withoutFacePanel = window.minimumSizeHint();
    const auto facePane = area->viewportLayout().paneIds(AreaRole::Inspector).front();
    area->setRole(facePane, AreaRole::FaceProperties);
    area->flush();
    app().processEvents();
    CHECK(window.minimumSizeHint().width() <= withoutFacePanel.width());
    CHECK(window.minimumSizeHint().height() <= withoutFacePanel.height());
    // Stacked areas whose minimum heights add up past the window must still show every header.
    const auto pane = facePane;
    auto layout = area->viewportLayout();
    REQUIRE(layout.split(pane, viewport::Orientation::Vertical));
    area->setLayout(std::move(layout));
    area->flush();
    app().processEvents();
    for (auto* b : area->findChildren<QToolButton*>("AreaRoleButton")) {
        const QRect inWindow(b->mapTo(&window, QPoint(0, 0)), b->size());
        CHECK(window.rect().contains(inWindow));
    }
}

TEST_CASE("UV view shift drag edits texture coordinates and Escape cancels the drag") {
    app();
    Scene scene;
    const auto top = scene.side({0, 0, 1});
    scene.session.selection().select(top);
    ui::FacePropertiesPanel panel;
    panel.resize(560, 420);
    panel.setDocument(&scene.doc);
    panel.setEditorSession(&scene.session);
    panel.show();
    app().processEvents();
    auto* view = panel.findChild<ui::UvView*>("faceUvView");
    REQUIRE(view);
    auto sendMouse = [&](QEvent::Type type, QPointF position, Qt::MouseButton button,
                         Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, position, view->mapToGlobal(position.toPoint()), button, buttons, modifiers);
        QApplication::sendEvent(view, &event);
    };
    const QPointF center = view->rect().center();
    // The face centre is a tile corner; start clear of the tile edges, which scale.
    const QPointF start = center + QPointF(21, 40), end = start + QPointF(30, 9);
    const auto before = editor::faceSnapshot(scene.doc, top)->face.uv;
    sendMouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    const auto shifted = editor::faceSnapshot(scene.doc, top)->face.uv;
    CHECK((shifted.shiftU != before.shiftU || shifted.shiftV != before.shiftV));
    const auto undo = scene.doc.undoCount();
    const QPointF cancelStart = center + QPointF(63, 37), cancelEnd = cancelStart + QPointF(18, 12);
    sendMouse(QEvent::MouseButtonPress, cancelStart, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    sendMouse(QEvent::MouseMove, cancelEnd, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(view, &escape);
    CHECK(scene.doc.undoCount() == undo);
    CHECK(editor::faceSnapshot(scene.doc, top)->face.uv.shiftU == shifted.shiftU);
    CHECK(editor::faceSnapshot(scene.doc, top)->face.uv.shiftV == shifted.shiftV);

    // An undo mid-drag takes the drag's entry; Esc then must not undo the earlier shift too.
    sendMouse(QEvent::MouseButtonPress, cancelStart, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    sendMouse(QEvent::MouseMove, cancelEnd, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    REQUIRE(scene.doc.undoCount() == undo + 1);
    scene.doc.undo();
    sendMouse(QEvent::MouseMove, cancelEnd + QPointF(5, 5), Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(view, &escape);
    CHECK(scene.doc.undoCount() == undo);
    CHECK(editor::faceSnapshot(scene.doc, top)->face.uv.shiftU == shifted.shiftU);
}

TEST_CASE("UV view rotate handle snaps to 15 degrees and a tile edge drag scales about the pivot") {
    app();
    Scene scene;
    const auto top = scene.side({0, 0, 1});
    scene.session.selection().select(top);
    ui::FacePropertiesPanel panel;
    panel.resize(560, 420);
    panel.setDocument(&scene.doc);
    panel.setEditorSession(&scene.session);
    panel.show();
    app().processEvents();
    auto* view = panel.findChild<ui::UvView*>("faceUvView");
    REQUIRE(view);
    const auto drag = [&](QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent press(QEvent::MouseButtonPress, from, view->mapToGlobal(from.toPoint()), Qt::LeftButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(view, &press);
        QMouseEvent move(QEvent::MouseMove, to, view->mapToGlobal(to.toPoint()), Qt::NoButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(view, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to, view->mapToGlobal(to.toPoint()), Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(view, &release);
    };
    // The pivot starts at the face centre, which the view frames at its centre; the handle is 42 px above.
    const QPointF pivot(view->width() / 2.0, view->height() / 2.0);
    const auto rotation = [&] { return editor::summarize(scene.doc, std::vector{top}).rotation.value(); };
    const auto undo = scene.doc.undoCount();
    drag(pivot + QPointF(0, -42), pivot + QPointF(40, -12)); // About 73° clockwise on screen: snaps to 75.
    CHECK(scene.doc.undoCount() == undo + 1);
    CHECK(rotation() == doctest::Approx(285));
    scene.doc.undo();
    drag(pivot + QPointF(0, -42), pivot + QPointF(40, -12), Qt::ShiftModifier);
    const auto free = rotation();
    CHECK(std::fmod(free, 15.0) != doctest::Approx(0));
    CHECK(free == doctest::Approx(360 - glm::degrees(std::atan2(40.0, 12.0))).epsilon(1e-3));
    scene.doc.undo();

    // Frame zoom as the view computes it, to find the first vertical tile edge right of the centre.
    const auto face = editor::faceSnapshot(scene.doc, top).value();
    double minX = INFINITY, maxX = -INFINITY, minY = INFINITY, maxY = -INFINITY;
    for (const auto p : face.points) {
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
    }
    const double zoom = std::min((view->width() - 32.0) / (maxX - minX), (view->height() - 32.0) / (maxY - minY));
    const auto [w, h] = scene.session.faceTextureSize(face.face.material);
    const auto& uv = face.face.uv;
    DVec3 centre{};
    for (const auto p : face.points) centre += p;
    centre /= static_cast<double>(face.points.size());
    // New solids are face aligned, so the texture axes are the view's axes: one texel is zoom * scale px.
    const double u0 = glm::dot(centre, uv.uaxis) / uv.scaleU + uv.shiftU;
    const double v0 = glm::dot(centre, uv.vaxis) / uv.scaleV + uv.shiftV;
    double edgeU = (std::floor(u0 / w) + 1) * w;
    if (zoom * uv.scaleU * (edgeU - u0) < 20) edgeU += w;
    const double midV = (std::floor(v0 / h) + 0.5) * h;
    const QPointF edge = pivot + QPointF(zoom * uv.scaleU * (edgeU - u0), zoom * uv.scaleV * (midV - v0));
    drag(edge, pivot + 2 * (edge - pivot) - QPointF(0, edge.y() - pivot.y()));
    const auto scaled = editor::faceSnapshot(scene.doc, top)->face.uv;
    CHECK(scaled.scaleU == doctest::Approx(face.face.uv.scaleU * 2).epsilon(0.02));
    CHECK(scaled.scaleV == doctest::Approx(face.face.uv.scaleV));
    const auto texel = [](const geometry::FaceUv& uv, DVec3 p) {
        return glm::dot(p, uv.uaxis) / uv.scaleU + uv.shiftU;
    };
    CHECK(std::remainder(texel(scaled, centre) - texel(face.face.uv, centre), w) == doctest::Approx(0).epsilon(1e-6));
}
