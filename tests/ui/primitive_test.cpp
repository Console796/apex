#include "test_app.hpp"
#include "../editor/fixtures.hpp"
#include "editor/editor_session.hpp"
#include "ui/main_window.hpp"
#include "ui/tool_parameter_strip.hpp"

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QToolBar>

using namespace editor_test;
TEST_CASE("primitive UI parameter strip drives tool fields and preview") {
    apex::ui_test::app();
    Document doc{parsed()};
    EditorSession session;
    session.attach(doc);
    REQUIRE(session.perform("tool.primitive"));
    apex::ui::ToolParameterStrip strip;
    auto& tool = session.activeToolObject();
    strip.update(tool);
    auto* type = strip.findChild<QComboBox*>(QStringLiteral("toolParameter_type"));
    REQUIRE(type);
    CHECK(type->count() == 5);
    auto* sides = strip.findChild<QDoubleSpinBox*>(QStringLiteral("toolParameter_sides"));
    REQUIRE(sides);
    sides->setValue(16);
    CHECK(tool.readout().parameters[1].value == 16);
    ToolView view;
    view.cursorRay = {{0, 0, 1000}, {0, 0, -1}};
    view.orthographic = true; view.cameraForward = {0, 0, -1};
    InputEvent e;
    e.kind = InputKind::Press; e.button = PointerButton::Left; e.source = 1; e.left = true;
    REQUIRE(session.input(e, view));
    e.kind = InputKind::Release; e.left = false;
    view.cursorRay.origin = {128, 128, 1000};
    REQUIRE(session.input(e, view));
    sides->setValue(12);
    REQUIRE(session.preview().ghosts().size() == 1);
    CHECK(session.preview().ghosts()[0].poly.faces.size() == 14);
    type->setCurrentIndex(3);
    REQUIRE(session.preview().ghosts().size() == 12);
    auto* width = strip.findChild<QDoubleSpinBox*>(QStringLiteral("toolParameter_width"));
    REQUIRE(width);
    width->setValue(8);
    CHECK(tool.readout().parameters[4].value == 8);
    // Schema changes retire old controls through Qt's deferred deletion queue.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    type = strip.findChild<QComboBox*>(QStringLiteral("toolParameter_type"));
    REQUIRE(type);
    type->setCurrentIndex(4);
    auto* segments = strip.findChild<QDoubleSpinBox*>(QStringLiteral("toolParameter_segments"));
    REQUIRE(segments);
    segments->setValue(6);
    CHECK(session.preview().ghosts().size() == 6);
    REQUIRE(session.perform("tool.block"));
    strip.update(session.activeToolObject());
    CHECK(strip.isHidden());
}
TEST_CASE("primitive action appears in the window tool list and toolbar") {
    apex::ui_test::app();
    apex::ui::MainWindow window;
    auto* action = window.findChild<QAction*>(QStringLiteral("tool.primitive"));
    REQUIRE(action);
    CHECK(action->isCheckable());
    auto* bar = window.findChild<QToolBar*>(QStringLiteral("toolbarTransform"));
    REQUIRE(bar);
    CHECK(bar->actions().contains(action));
}
