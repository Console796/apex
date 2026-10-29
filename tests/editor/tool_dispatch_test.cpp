#include "fixtures.hpp"
#include "editor/editor_session.hpp"

using namespace editor_test;
using namespace std::chrono_literals;

namespace {
ToolView viewFor(Ray ray = {{-24, -24, 1000}, {0, 0, -1}}) {
    ToolView view;
    view.cursorRay = ray;
    view.size = {800, 600};
    view.cameraPosition = {0, 0, 1000};
    view.cameraAngles = {90, 0, 0};
    view.cameraForward = {0, 0, -1};
    view.cameraUp = {0, 1, 0};
    view.orthographic = true;
    view.orthoHeight = 600;
    return view;
}
InputEvent pointer(InputKind kind, DVec2 cursor = {100, 100}, InputSource source = 1) {
    InputEvent event;
    event.kind = kind;
    event.source = source;
    event.cursor = cursor;
    event.button = kind == InputKind::Move ? PointerButton::None : PointerButton::Left;
    event.left = kind == InputKind::Press;
    return event;
}
InputEvent shortcut(std::string_view value, InputKind kind = InputKind::KeyDown) {
    InputEvent event;
    event.kind = kind;
    event.source = 1;
    event.cursor = {100, 100};
    event.chord = KeyChord::parse(value).value();
    return event;
}
void clickAt(EditorSession& session, const ToolView& view, InputSource source = 1) {
    session.input(pointer(InputKind::Press, {100, 100}, source), view);
    session.input(pointer(InputKind::Release, {100, 100}, source), view);
}
struct Scene {
    Document doc{parsed()};
    NodeId box = id(doc, addBox(doc, {-32, -32, -32}, {32, 32, 0}));
    EditorSession session;
    Scene() { session.attach(doc); }
};
}

TEST_CASE("tool dispatch routes pointer input only to the active tool") {
    Scene scene;
    auto& session = scene.session;
    auto& select = session.activeToolObject();
    CHECK(select.id() == "tool.select");
    clickAt(session, viewFor());
    CHECK(session.selection().contains(scene.box));
    session.input(pointer(InputKind::Move), viewFor());
    REQUIRE(session.hoverTarget());
    auto leave = pointer(InputKind::Leave);
    session.input(leave, viewFor());
    CHECK_FALSE(session.hoverTarget());
    const auto history = scene.doc.undoCount();
    assets::EntityClassTable classes;
    assets::EntityClass cls;
    cls.name = "test";
    cls.type = "PointClass";
    classes.classes[cls.name] = cls;
    session.setEntityClasses(std::move(classes));
    session.setCurrentEntityClass("test");
    REQUIRE(session.perform("tool.entity"));
    CHECK(session.activeToolObject().id() == "tool.entity");
    CHECK(&select != &session.activeToolObject());
    session.selection().clear();
    session.setCurrentEntityClass("");
    clickAt(session, viewFor());
    CHECK(session.selection().ordered().empty()); // Inactive Select never sees this click.
    CHECK(scene.doc.undoCount() == history);
    session.selectNode(scene.box);
    session.setCurrentEntityClass("test");
    session.input(pointer(InputKind::Move), viewFor());
    CHECK_FALSE(session.hoverTarget());
    clickAt(session, viewFor());
    CHECK(scene.doc.undoCount() == history + 1);
    CHECK_FALSE(session.selection().contains(scene.box));
    CHECK(select.preview().empty());
    REQUIRE(session.perform("edit.undo"));
    CHECK(session.selection().contains(scene.box));
    REQUIRE(session.perform("tool.select"));
    CHECK(&select == &session.activeToolObject());
    CHECK(scene.doc.undoCount() == history);
}

TEST_CASE("session pie consumes input before the active tool and performs its pick") {
    Scene swallowed;
    REQUIRE(swallowed.session.perform("tool.block"));
    auto open = shortcut("Q");
    REQUIRE(swallowed.session.input(open, viewFor()));
    REQUIRE(swallowed.session.pie());
    CHECK(swallowed.session.pieSource() == 1);
    auto toolShortcut = shortcut("Shift+S");
    CHECK(swallowed.session.input(toolShortcut, viewFor()));
    CHECK(swallowed.session.activeTool() == "tool.block");
    CHECK(swallowed.session.input(shortcut("Q", InputKind::KeyUp), viewFor()));
    CHECK(swallowed.session.pie());
    CHECK(swallowed.session.input(toolShortcut, viewFor()));
    CHECK(swallowed.session.pie());
    CHECK(swallowed.session.input(pointer(InputKind::Press, {100, 100}), viewFor()));
    CHECK_FALSE(swallowed.session.activeToolObject().owns(1));
    CHECK_FALSE(swallowed.session.pie());

    Scene picked;
    std::chrono::milliseconds now{0};
    picked.session.setClock([&] { return now; });
    open = shortcut("Q");
    REQUIRE(picked.session.input(open, viewFor()));
    now = 10ms;
    CHECK(picked.session.input(pointer(InputKind::Move, {200, 100}), viewFor()));
    auto release = shortcut("Q", InputKind::KeyUp);
    now = 50ms;
    CHECK(picked.session.input(release, viewFor()));
    CHECK_FALSE(picked.session.pie());
    CHECK_FALSE(picked.session.pieSource());
    CHECK(picked.session.activeTool() == "tool.block");
}

TEST_CASE("session refuses pie while a block gesture or modal transform owns the pane") {
    Scene block;
    REQUIRE(block.session.perform("tool.block"));
    auto press = pointer(InputKind::Press);
    CHECK(block.session.input(press, viewFor()));
    REQUIRE(block.session.activeToolObject().owns(1));
    auto open = shortcut("Q");
    CHECK(block.session.input(open, viewFor()));
    CHECK_FALSE(block.session.pie());
    CHECK(block.session.pieRefusal() == "Finish the current gesture first.");

    Scene modal;
    modal.session.selectNode(modal.box);
    const auto view = viewFor();
    const auto context = *modal.session.gizmoContext();
    REQUIRE(modal.session.beginModal(GizmoMode::Translate, gizmoViewFor(view, context.pivot), view.cursorRay));
    REQUIRE(modal.session.transformActive());
    CHECK(modal.session.input(open, view));
    CHECK_FALSE(modal.session.pie());
    CHECK(modal.session.pieRefusal() == "Finish the current gesture first.");

    auto navigating = shortcut("Q");
    navigating.navigating = true;
    CHECK(modal.session.input(navigating, view));
    CHECK_FALSE(modal.session.pie());
    CHECK(modal.session.pieRefusal() == "Stop navigating first.");
}

TEST_CASE("session pie settings and clock drive confirm on stop; focus loss closes it") {
    Scene scene;
    std::chrono::milliseconds now{0};
    scene.session.setClock([&] { return now; });
    PieSettings settings;
    settings.confirmOnStopDistance = 50;
    scene.session.setPieSettings(settings);
    auto open = shortcut("Q");
    REQUIRE(scene.session.input(open, viewFor()));
    CHECK(scene.session.pie());
    auto move = pointer(InputKind::Move, {200, 100});
    scene.session.input(move, viewFor());
    now = 20ms;
    scene.session.tickPie();
    CHECK_FALSE(scene.session.pie());
    CHECK(scene.session.activeTool() == "tool.block");

    REQUIRE(scene.session.input(open, viewFor()));
    scene.session.pieFocusLost();
    CHECK_FALSE(scene.session.pie());
    CHECK_FALSE(scene.session.pieSource());
}

TEST_CASE("session pie without a document enables only Select") {
    EditorSession session;
    session.setActiveTool("tool.block");
    auto open = shortcut("Q");
    REQUIRE(session.input(open, {}));
    REQUIRE(session.pie());
    auto block = shortcut("6");
    CHECK(session.input(block, {}));
    CHECK(session.pie());
    CHECK(session.activeTool() == "tool.block");
    auto select = shortcut("4");
    CHECK(session.input(select, {}));
    CHECK_FALSE(session.pie());
    CHECK(session.activeTool() == "tool.select");
}

TEST_CASE("tool switch cancels the previous gesture and clears its preview") {
    Scene scene;
    auto& session = scene.session;
    session.selectNode(scene.box);
    const auto bytes = *scene.doc.write();
    const auto history = scene.doc.undoCount();
    auto& select = session.activeToolObject();
    const auto context = *session.gizmoContext();
    const auto view = viewFor();
    REQUIRE(session.beginModal(GizmoMode::Translate, gizmoViewFor(view, context.pivot), view.cursorRay));
    session.modalKey("X");
    session.modalKey("3");
    session.modalKey("2");
    CHECK_FALSE(select.preview().empty());
    session.setActiveTool(EditorSession::Tool::Entity);
    CHECK_FALSE(session.transformActive());
    CHECK(select.preview().empty());
    CHECK(session.preview().empty());
    session.setActiveTool(EditorSession::Tool::Select);
    CHECK_FALSE(session.transformActive());
    CHECK(*scene.doc.write() == bytes);
    CHECK(scene.doc.undoCount() == history);

    // A pending click cannot fire on release after a tool switch either.
    session.selection().clear();
    session.input(pointer(InputKind::Press), viewFor());
    session.setActiveTool(EditorSession::Tool::Entity);
    session.setActiveTool(EditorSession::Tool::Select);
    session.input(pointer(InputKind::Release), viewFor());
    CHECK(session.selection().ordered().empty());
}

TEST_CASE("tool input click and marquee match the direct session APIs") {
    Scene input, direct;
    addBox(input.doc, {64, -32, -32}, {96, 32, 0});
    addBox(direct.doc, {64, -32, -32}, {96, 32, 0});
    const auto before = input.doc.undoCount();
    clickAt(input.session, viewFor());
    direct.session.pointerClick(viewFor().cursorRay);
    CHECK(input.session.selection().ordered() == direct.session.selection().ordered());

    const auto frustum = planes({-40, -40, -40}, {100, 40, 10});
    auto view = viewFor({{-100, -100, 1000}, {0, 0, -1}});
    view.rectFrustum = [&](DVec2 start, DVec2 end) {
        CHECK(start == DVec2(20, 20));
        CHECK(end == DVec2(120, 120));
        return std::array<Plane, 4>{frustum[0], frustum[1], frustum[2], frustum[3]};
    };
    // Use the same four side planes in both paths (marquee has unbounded depth).
    const auto expected = view.rectFrustum({20, 20}, {120, 120});
    input.session.input(pointer(InputKind::Press, {20, 20}), view);
    auto move = pointer(InputKind::Move, {23, 20});
    move.left = true;
    input.session.input(move, view);
    CHECK_FALSE(input.session.activeToolObject().overlay(view, 1).rectangle);
    move.cursor = {120, 120};
    input.session.input(move, view);
    CHECK(input.session.activeToolObject().overlay(view, 1).rectangle.has_value());
    input.session.input(pointer(InputKind::Release, {120, 120}), view);
    direct.session.marquee(expected);
    CHECK(input.session.selection().ordered() == direct.session.selection().ordered());
    CHECK(input.session.selection().ordered().size() == 2);
    CHECK_FALSE(input.session.activeToolObject().overlay(view, 1).rectangle);
    CHECK(input.doc.undoCount() == before);
}

TEST_CASE("tool pointer cycling is per pane and camera changes cancel pending clicks") {
    Scene scene;
    const auto lower = id(scene.doc, addBox(scene.doc, {-32, -32, -96}, {32, 32, -64}));
    auto view = viewFor();
    clickAt(scene.session, view);
    CHECK(scene.session.selection().contains(scene.box));
    clickAt(scene.session, view);
    CHECK(scene.session.selection().contains(lower));
    // A different pane starts a fresh cycle at the front candidate.
    clickAt(scene.session, view, 2);
    CHECK(scene.session.selection().contains(scene.box));
    scene.session.selection().clear();
    scene.session.input(pointer(InputKind::Press), view);
    view.cameraPosition.x += 1;
    scene.session.input(pointer(InputKind::Release), view);
    CHECK(scene.session.selection().ordered().empty());
    scene.session.input(pointer(InputKind::Press), view);
    view.cameraAngles.y += 1;
    scene.session.input(pointer(InputKind::Move, {120, 120}), view);
    scene.session.input(pointer(InputKind::Release), view);
    CHECK(scene.session.selection().ordered().empty());
}

TEST_CASE("tool input gizmo drag matches the direct API and commits one undo step") {
    Scene input, direct;
    input.session.selectNode(input.box);
    direct.session.selectNode(direct.box);
    input.session.setGridSize(16);
    direct.session.setGridSize(16);
    auto view = viewFor();
    const auto context = *input.session.gizmoContext();
    const auto gizmo = gizmoViewFor(view, context.pivot);
    DVec3 start{0.0};
    for (const auto& p : gizmoDescription(GizmoMode::Translate, context, gizmo))
        if (p.handle == GizmoHandle::X) start = (p.points[0] + p.points[1]) * 0.5;
    view.cursorRay = {{start.x, start.y, 1000}, {0, 0, -1}};
    const auto hit = hitTestGizmo(GizmoMode::Translate, context, gizmo, view.cursorRay);
    REQUIRE(hit);
    REQUIRE(hit->handle == GizmoHandle::X);
    const auto history = input.doc.undoCount();
    const auto before = *input.doc.write();
    REQUIRE(input.session.input(pointer(InputKind::Press), view));
    REQUIRE(input.session.activeToolObject().owns(1));
    REQUIRE(direct.session.beginGizmoDrag(GizmoHandle::X, gizmo, view.cursorRay));
    view.cursorRay.origin.x += 35;
    auto move = pointer(InputKind::Move, {135, 100});
    move.left = true;
    move.alt = true;
    REQUIRE(input.session.input(move, view));
    REQUIRE(direct.session.updateGizmoDrag(view.cursorRay, {.suspend = true}));
    CHECK(input.session.preview().transforms() == direct.session.preview().transforms());
    CHECK(input.doc.undoCount() == history);
    // Another pane cannot update the transform owned by pane 1.
    auto other = move;
    other.source = 2;
    auto otherView = view;
    otherView.cursorRay.origin.x += 100;
    input.session.input(other, otherView);
    CHECK(input.session.preview().transforms() == direct.session.preview().transforms());
    auto release = pointer(InputKind::Release, {135, 100});
    release.alt = true;
    REQUIRE(input.session.input(release, view));
    REQUIRE(direct.session.endGizmoDrag());
    CHECK(input.doc.undoCount() == history + 1);
    CHECK(*input.doc.write() == *direct.doc.write());
    CHECK(input.session.preview().empty());
    REQUIRE(input.session.perform("edit.undo"));
    CHECK(*input.doc.write() == before);
    CHECK(input.session.selection().contains(input.box));
}

TEST_CASE("tool input modal keys retain preset snapping and cancel semantics") {
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) {
        Scene scene;
        scene.session.setPreset(*preset);
        scene.session.selectNode(scene.box);
        const auto history = scene.doc.undoCount();
        const auto bytes = *scene.doc.write();
        const auto view = viewFor();
        InputEvent key;
        key.kind = InputKind::KeyDown;
        key.source = 1;
        key.chord = KeyChord{"G"};
        if (preset == &hammerPreset()) {
            CHECK_FALSE(scene.session.input(key, view));
            CHECK_FALSE(scene.session.transformActive());
        } else {
            REQUIRE(scene.session.input(key, view));
            CHECK(scene.session.modal().active());
            key.chord.reset();
            for (const auto* name : {"X", "3", "2"}) {
                key.key = name;
                REQUIRE(scene.session.input(key, view));
            }
            CHECK_FALSE(scene.session.activeToolObject().readout().status.empty());
            key.key = "Escape";
            REQUIRE(scene.session.input(key, view));
            CHECK(scene.session.preview().empty());
        }
        CHECK(*scene.doc.write() == bytes);
        CHECK(scene.doc.undoCount() == history);
        InputEvent mods;
        mods.alt = true;
        CHECK(transformModifiers(*preset, mods).suspend == (preset == &hammerPreset()));
        mods.alt = false;
        mods.ctrl = true;
        CHECK(transformModifiers(*preset, mods).suspend == (preset == &blenderPreset()));
    }
}

TEST_CASE("tool dispatch keeps hover still while an unlisted button is held") {
    Scene scene;
    auto& session = scene.session;
    auto move = pointer(InputKind::Move);
    move.anyButton = true; // Back/forward: no field of its own.
    session.input(move, viewFor());
    CHECK_FALSE(session.hoverTarget());
    session.input(pointer(InputKind::Move), viewFor());
    CHECK(session.hoverTarget());
}

TEST_CASE("tool dispatch forgets a closed pane's pending gesture") {
    Scene scene;
    auto& session = scene.session;
    session.input(pointer(InputKind::Press, {100, 100}, 2), viewFor());
    session.forgetInputSource(2);
    session.input(pointer(InputKind::Release, {100, 100}, 2), viewFor());
    CHECK(session.selection().ordered().empty());
    clickAt(session, viewFor(), 2);
    CHECK(session.selection().contains(scene.box));
}

TEST_CASE("pie dispatch swallows a held trigger release after digit click escape and focus loss") {
    for (const int how : {0, 1, 2, 3}) {
        Scene scene;
        auto& session = scene.session;
        REQUIRE(session.input(shortcut("Q"), viewFor()));
        if (how == 0) session.input(shortcut("6"), viewFor());
        if (how == 1) session.input(shortcut("Escape"), viewFor());
        if (how == 2) session.pieFocusLost();
        if (how == 3) session.input(pointer(InputKind::Press, {200, 100}), viewFor());
        REQUIRE_FALSE(session.pie());
        auto release = shortcut("Q", InputKind::KeyUp);
        release.source = 99;
        CHECK(session.input(release, viewFor()));
        CHECK_FALSE(session.input(release, viewFor()));
    }
}

namespace {
assets::EntityClassTable lightClasses() {
    assets::EntityClassTable table;
    auto& cls = table.classes["light_rt_spot"];
    cls.name = "light_rt_spot";
    cls.type = "PointClass";
    cls.helpers = {{"lightprop", "\"models/editor/spot.mdl\""}, {"lightcone", ""},
        {"sphere", "_fifty_percent_distance"}, {"sphere", "_zero_percent_distance"}};
    for (const auto& [key, value] : {std::pair{"_light", "255 255 255 200"}, {"_fifty_percent_distance", "128"},
             {"_zero_percent_distance", "256"}, {"_distance", "0"}, {"_inner_cone", "30"}, {"_cone", "45"}, {"pitch", "0"}})
        cls.properties.push_back({.name = key, .type = "float", .defaultValue = value});
    return table;
}
}
TEST_CASE("light handles preview falloff with one undoable commit and cancel without changes") {
    Document doc(parsed(R"(entity { id 1 classname light_rt_spot origin "0 0 0" })"));
    EditorSession session;
    session.attach(doc);
    session.setGridSize(8);
    session.setEntityClasses(lightClasses());
    session.selectNode(doc.roots()[0]);
    const auto initial = *doc.write();
    const auto history = doc.undoCount();
    const auto at = [&](InputKind kind, DVec3 point, bool shift = false) {
        auto view = viewFor({point + DVec3{0, 0, 1000}, {0, 0, -1}});
        auto e = pointer(kind, *worldToPixel(view, point));
        e.shift = shift;
        return session.input(e, view);
    };
    CHECK(at(InputKind::Press, {128, 0, 0}));
    CHECK(session.transformActive());
    CHECK(session.activeToolObject().owns(1));
    CHECK(at(InputKind::Move, {160, 0, 0}));
    REQUIRE(session.preview().entityKeys().contains(doc.roots()[0]));
    CHECK(session.preview().entityKeys().at(doc.roots()[0]).at("_fifty_percent_distance") == "160");
    CHECK(*doc.write() == initial);
    CHECK(doc.undoCount() == history);
    CHECK(at(InputKind::Release, {160, 0, 0}));
    CHECK_FALSE(session.transformActive());
    CHECK(session.preview().empty());
    CHECK(doc.undoCount() == history + 1);
    const auto& block = *doc.get(*doc.handle(doc.roots()[0]))->block;
    CHECK(block.find("_fifty_percent_distance")->asDouble().value() == 160);
    REQUIRE(session.perform("edit.undo"));
    CHECK(*doc.write() == initial);
    CHECK(at(InputKind::Press, {128, 0, 0}));
    CHECK(at(InputKind::Move, {158, 0, 0}, true));
    CHECK(session.preview().entityKeys().at(doc.roots()[0]).at("_fifty_percent_distance") == "131");
    auto escape = pointer(InputKind::KeyDown);
    escape.key = "Escape";
    CHECK(session.input(escape, viewFor()));
    CHECK_FALSE(session.transformActive());
    CHECK(session.preview().empty());
    CHECK(*doc.write() == initial);
    // Clicking a handle must not materialize defaults or snap a value without a drag.
    CHECK(at(InputKind::Press, {128, 0, 0}));
    CHECK(at(InputKind::Release, {128, 0, 0}));
    CHECK(*doc.write() == initial);
    CHECK_FALSE(session.transformActive());
    CHECK(at(InputKind::Press, {128, 0, 0}));
    CHECK(at(InputKind::Move, {160, 0, 0}));
    session.selection().clear();
    CHECK_FALSE(session.transformActive());
    CHECK(session.preview().empty());
    CHECK(*doc.write() == initial);
}
TEST_CASE("light fine controls preserve valid falloff and cone bounds") {
    Document doc(parsed(R"(entity { id 1 classname light_rt_spot origin "0 0 0" })"));
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(lightClasses());
    session.selectNode(doc.roots()[0]);
    auto& tool = session.activeToolObject();
    REQUIRE(tool.readout().parameters.size() == 5);
    const auto history = doc.undoCount();
    REQUIRE(tool.setParameter("_fifty_percent_distance", 300.25));
    CHECK(doc.undoCount() == history + 1);
    const auto block = [&]() { return doc.get(*doc.handle(doc.roots()[0]))->block; };
    CHECK(block()->find("_fifty_percent_distance")->asDouble().value() == 300.25);
    CHECK(block()->find("_zero_percent_distance")->asDouble().value() == 600.5);
    REQUIRE(tool.setParameter("_inner_cone", 60));
    CHECK(block()->find("_inner_cone")->asDouble().value() == 60);
    CHECK(block()->find("_cone")->asDouble().value() == 60);
    REQUIRE(tool.setParameter("_distance", 0));
    CHECK(block()->find("_distance")->asDouble().value() == 0);
    const auto view = viewFor();
    CHECK_FALSE(tool.overlay(view, 1).worldPoints.empty());
}
TEST_CASE("light cone handles change both bounds when dragged inside the bright cone") {
    Document doc(parsed(R"(entity { id 1 classname light_rt_spot origin "0 0 0" })"));
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(lightClasses());
    session.selectNode(doc.roots()[0]);
    const auto initial = *doc.write();
    const auto dragAt = [&](InputKind kind, DVec3 point) {
        auto view = viewFor({point + DVec3{0, 0, 1000}, {0, 0, -1}});
        return session.input(pointer(kind, *worldToPixel(view, point)), view);
    };
    REQUIRE(dragAt(InputKind::Press, {256, 256, 0}));
    REQUIRE(dragAt(InputKind::Move, {256, 128, 0}));
    REQUIRE(session.preview().entityKeys().contains(doc.roots()[0]));
    CHECK(session.preview().entityKeys().at(doc.roots()[0]).at("_cone") == "27");
    CHECK(session.preview().entityKeys().at(doc.roots()[0]).at("_inner_cone") == "27");
    CHECK(*doc.write() == initial);
    REQUIRE(dragAt(InputKind::Release, {256, 128, 0}));
    CHECK(doc.undoCount() == 1);
    const auto* block = doc.get(*doc.handle(doc.roots()[0]))->block.get();
    CHECK(block->find("_cone")->asDouble().value() == 27);
    CHECK(block->find("_inner_cone")->asDouble().value() == 27);
}
TEST_CASE("spotlight rotations update the separate pitch override used by compiled lights") {
    Document doc(parsed(R"(entity { id 1 classname light_rt_spot origin "0 0 0" angles "0 0 0" pitch "-30" })"));
    const auto classes = lightClasses();
    const auto rotated = applyToEntity(doc, doc.roots()[0], Transform::rotate({0, 1, 0}, 30), true, &classes);
    REQUIRE(rotated);
    REQUIRE(rotated->angles);
    REQUIRE(rotated->keys.contains("pitch"));
    CHECK(rotated->angles->x == doctest::Approx(60));
    CHECK(std::stod(rotated->keys.at("pitch")) == doctest::Approx(-60));
}
