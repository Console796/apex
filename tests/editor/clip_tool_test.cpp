#include "corpus.hpp"
#include "editor/editor_session.hpp"
#include "geometry/face_matching.hpp"
#include "geometry/solid_block.hpp"

using namespace editor_test;

namespace {
InputEvent event(InputKind kind, InputSource source = 1) {
    InputEvent e;
    e.kind = kind;
    e.source = source;
    e.button = PointerButton::Left;
    e.left = kind == InputKind::Press;
    return e;
}
ToolView viewAt(DVec3 point, DVec3 direction = {-1, 0, 0}, bool ortho = false) {
    ToolView view;
    view.cursorRay = {point - direction * 1000.0, direction};
    view.cameraPosition = view.cursorRay.origin;
    view.cameraForward = direction;
    view.cameraUp = std::abs(direction.z) > 0.9 ? DVec3{0, 1, 0} : DVec3{0, 0, 1};
    view.size = {800, 600};
    view.orthographic = ortho;
    view.orthoHeight = 600;
    return view;
}
void key(EditorSession& session, std::string name, InputSource source = 1) {
    auto e = event(InputKind::KeyDown, source);
    e.key = std::move(name);
    REQUIRE(session.input(e, {}));
}
void place(EditorSession& session, DVec3 p, DVec3 direction = {-1, 0, 0}, bool ortho = false,
           InputSource source = 1, bool suspended = true) {
    SurfacePlane plane;
    plane.normal = -direction;
    plane.origin = plane.normal * glm::dot(p, plane.normal);
    plane.uAxis = std::abs(direction.z) > 0.9 ? DVec3{1, 0, 0} : glm::normalize(glm::cross(DVec3{0, 0, 1}, direction));
    plane.vAxis = glm::cross(plane.normal, plane.uAxis);
    session.pinWorkPlane(plane);
    auto e = event(InputKind::Press, source);
    e.alt = suspended && session.preset().name == "Hammer";
    e.ctrl = suspended && session.preset().name == "Blender";
    REQUIRE(session.input(e, viewAt(p, direction, ortho)));
    e.kind = InputKind::Release; e.left = false;
    REQUIRE(session.input(e, viewAt(p, direction, ortho)));
}
std::vector<Handle<Solid>> solids(const Document& doc) {
    std::vector<Handle<Solid>> result;
    for (const auto h : nodes(doc)) if (const auto* solid = std::get_if<Handle<Solid>>(&h)) result.push_back(*solid);
    return result;
}
struct Scene {
    Document doc{parsed()};
    EditorSession session;
    Handle<Solid> solid;
    Scene() {
        solid = std::get<Handle<Solid>>(addBox(doc, {-64, -64, -64}, {64, 64, 64}));
        session.attach(doc);
        session.selection().setMode(SelectionMode::Solids);
        session.selection().select(doc.get(solid)->id);
        REQUIRE(session.perform("tool.clip"));
    }
};
void planeX(EditorSession& session, double x = 0, bool ortho = false) {
    const DVec3 direction = ortho ? DVec3{0, 0, -1} : DVec3{-1, 0, 0};
    place(session, {x, -200, -200}, direction, ortho);
    place(session, {x, 200, -200}, direction, ortho);
    if (!ortho) place(session, {x, -200, 200});
}
std::vector<std::string> fields(const kv1::Node& node, bool fragment = false) {
    std::vector<std::string> result;
    for (const auto& child : node.children()) {
        if (child.key() == "plane" || child.key() == "point_data" || (fragment && child.key() == "id")) continue;
        result.emplace_back(child.rawKey());
        result.emplace_back(child.rawValue().value_or(""));
        result.emplace_back(child.condition());
        const auto nested = fields(child, false);
        result.insert(result.end(), nested.begin(), nested.end());
    }
    return result;
}
void checkClip(const kv1::Node& block) {
    for (int mode = 0; mode < 3; ++mode) {
        CAPTURE(mode);
        Document doc{parsed()};
        auto tx = doc.begin("fixture");
        const auto handle = std::get<Handle<Solid>>(tx.create(block));
        tx.commit();
        const auto original = *doc.get(handle)->block;
        const auto source = geometry::solidGeometry(original);
        REQUIRE(source);
        const auto bounds = source->poly.bounds();
        const auto centre = bounds.center();
        const double radius = glm::length(bounds.max - bounds.min) + 100;
        EditorSession session;
        session.attach(doc);
        session.selection().setMode(SelectionMode::Solids);
        session.selection().select(doc.get(handle)->id);
        REQUIRE(session.perform("tool.clip"));
        for (int i = 0; i < mode; ++i) REQUIRE(session.perform("clip.cycle_keep"));
        const auto bytes = *doc.write();
        const auto undo = doc.undoCount();
        place(session, centre + DVec3{0, -radius, -radius});
        place(session, centre + DVec3{0, radius, -radius});
        place(session, centre + DVec3{0, -radius, radius});
        const Plane plane{{1, 0, 0}, centre.x};
        const auto split = geometry::split(source->poly, plane);
        REQUIRE(split.front); REQUIRE(split.back);
        CHECK(split.front->volume() + split.back->volume() == doctest::Approx(source->poly.volume()).epsilon(1e-7));
        REQUIRE(session.preview().replacements().size() == 1);
        const auto replacement = session.preview().replacements().begin()->second;
        const auto ghost = session.preview().ghosts()[0].poly;
        key(session, "Enter");
        CHECK(doc.undoCount() == undo + 1);
        const auto output = solids(doc);
        REQUIRE(output.size() == (mode == 2 ? 2 : 1));
        double volume = 0;
        for (const auto solid : output) {
            const bool fragment = solid != handle;
            const auto& written = *doc.get(solid)->block;
            const auto result = geometry::solidGeometry(written);
            REQUIRE_MESSAGE(result, (result ? "" : result.error().what()));
            CHECK(result->poly.validate().empty());
            geometry_test::sameVertices(result->poly, fragment ? ghost : replacement);
            volume += result->poly.volume();
            auto copy = result->poly;
            const auto matches = geometry::matchFaces(source->poly, copy);
            const auto oldSides = original.findAll("side");
            const auto newSides = written.findAll("side");
            for (const auto& match : matches.matched) {
                const auto oldId = source->poly.faces[match.oldIndex].sideId;
                const auto newId = result->poly.faces[match.newIndex].sideId;
                const auto old = std::ranges::find_if(oldSides, [&](auto* s) { return s->find("id")->asInt() == static_cast<int>(oldId); });
                const auto now = std::ranges::find_if(newSides, [&](auto* s) { return s->find("id")->asInt() == static_cast<int>(newId); });
                REQUIRE(old != oldSides.end()); REQUIRE(now != newSides.end());
                CHECK(fields(**now, fragment) == fields(**old, fragment));
            }
            for (const auto& cap : matches.unmatchedNew) {
                const auto donorId = source->poly.faces[*cap.inheritFrom].sideId;
                const auto donorIt = std::ranges::find_if(oldSides, [&](auto* side) { return side->find("id")->asInt() == static_cast<int>(donorId); });
                REQUIRE(donorIt != oldSides.end());
                const auto* donor = *donorIt;
                const auto* newSide = newSides[cap.newIndex];
                for (const auto field : {"material", "uaxis", "vaxis"})
                    if (donor->find(field)) CHECK(newSide->find(field)->decodedValue() == donor->find(field)->decodedValue().value());
            }
        }
        const double expected = mode == 2 ? source->poly.volume() : mode == 0 ? split.front->volume() : split.back->volume();
        CHECK(volume == doctest::Approx(expected).epsilon(1e-7));
        REQUIRE(session.perform("edit.undo"));
        CHECK(*doc.write() == bytes);
    }
}
}

TEST_CASE("clip fixture modes preserve geometry side payloads caps and undo bytes") {
    auto poly = geometry_test::build(geometry_test::boxSides({-64, -48, -32}, {64, 48, 32}));
    auto block = geometry::solidBlock(poly, {"clip/material"});
    for (auto* side : block.findAll("side")) {
        REQUIRE(side->append(kv1::Node::value("unknown", "keep")));
        REQUIRE(side->append(kv1::Node::value("unknown", "duplicate")));
    }
    checkClip(block);
}

TEST_CASE("clip every corpus solid in every keep mode") {
    const auto paths = corpusPaths();
    if (paths.empty()) { MESSAGE("APEX_P2CE_DIR unset; skipping clip corpus"); return; }
    std::size_t count = 0;
    for (const auto& path : paths) {
        CAPTURE(path.string());
        Document doc{readCorpus(path)};
        for (const auto solid : solids(doc)) {
            checkClip(*doc.get(solid)->block);
            ++count;
        }
    }
    CHECK(count > 0);
    MESSAGE("Clipped ", count, " corpus solids in three keep modes");
}

TEST_CASE("clip two points in each axis projection equal three point planes") {
    for (int axis = 0; axis < 3; ++axis) {
        std::vector<geometry::ConvexPolyhedron> results;
        for (bool ortho : {true, false}) {
            Scene scene;
            DVec3 direction{0}, a{0}, b{0}, c{0};
            direction[axis] = -1;
            a[(axis + 1) % 3] = -200;
            b[(axis + 1) % 3] = 200;
            c = a + direction * 200.0;
            place(scene.session, a, direction, ortho);
            place(scene.session, b, direction, ortho);
            if (!ortho) {
                CHECK(scene.session.preview().empty());
                // View the third point from outside the brush along another axis.
                DVec3 thirdDirection{0}; thirdDirection[(axis + 2) % 3] = -1;
                place(scene.session, c, thirdDirection);
            }
            REQUIRE(scene.session.preview().replacements().size() == 1);
            key(scene.session, "Enter");
            results.push_back(geometry::solidGeometry(*scene.doc.get(scene.solid)->block)->poly);
        }
        geometry_test::sameVertices(results[0], results[1]);
    }
}

TEST_CASE("clip headless placement handle drag keep cycling back steps and commit") {
    Scene scene;
    const auto before = *scene.doc.write();
    const auto undo = scene.doc.undoCount();
    planeX(scene.session, 0, true);
    REQUIRE(scene.session.preview().replacements().size() == 1);
    auto press = event(InputKind::Press);
    press.alt = true;
    REQUIRE(scene.session.input(press, viewAt({0, -200, -200}, {0, 0, -1}, true)));
    auto move = press; move.kind = InputKind::Move;
    REQUIRE(scene.session.input(move, viewAt({0, -160, -200}, {0, 0, -1}, true)));
    auto release = move; release.kind = InputKind::Release; release.left = false;
    REQUIRE(scene.session.input(release, viewAt({0, -160, -200}, {0, 0, -1}, true)));
    const auto overlay = scene.session.activeToolObject().overlay({}, 1);
    REQUIRE(overlay.worldPoints.size() == 2);
    checkPoint(overlay.worldPoints[0].position, {0, -160, -200});
    checkPoint(overlay.labels.back().position, overlay.worldPoints[0].position);
    CHECK_FALSE(scene.session.input(event(InputKind::CancelPointer), {}));
    REQUIRE(scene.session.perform("tool.clip"));
    CHECK(scene.session.activeToolObject().readout().status.starts_with("Keep back"));
    auto cycle = event(InputKind::KeyDown);
    cycle.chord = *KeyChord::parse("Shift+X");
    REQUIRE(scene.session.input(cycle, {}));
    CHECK(scene.session.activeToolObject().readout().status.starts_with("Keep both"));
    auto right = event(InputKind::Press); right.button = PointerButton::Right; right.navigating = true;
    REQUIRE(scene.session.input(right, {}));
    CHECK(scene.session.preview().empty());
    REQUIRE(scene.session.activeToolObject().overlay({}, 1).worldPoints.size() == 1);
    key(scene.session, "Escape");
    CHECK_FALSE(scene.session.activeToolObject().owns(1));
    CHECK(scene.doc.undoCount() == undo);
    planeX(scene.session);
    key(scene.session, "Enter");
    CHECK(solids(scene.doc).size() == 2);
    CHECK(scene.doc.undoCount() == undo + 1);
    CHECK(scene.session.preview().empty());
    REQUIRE(scene.session.perform("edit.undo"));
    CHECK(*scene.doc.write() == before);
}

TEST_CASE("clip both preserves group two visgroups and brush entity parent") {
    Scene scene;
    auto tx = scene.doc.begin("memberships");
    const auto entity = tx.create(block("entity { classname func_detail }"));
    tx.reparent(scene.solid, entity);
    const auto group = tx.create(block("group { id 31 }"));
    const auto first = tx.create(block("visgroup { visgroupid 41 name first }"));
    const auto second = tx.create(block("visgroup { visgroupid 42 name second }"));
    tx.setEditor(scene.solid, EditorEdit::setGroup(scene.doc.get(group)->block->find("id")->asInt().value()));
    tx.setEditor(scene.solid, EditorEdit::addVisgroup(scene.doc.get(first)->block->find("visgroupid")->asInt().value()));
    tx.setEditor(scene.solid, EditorEdit::addVisgroup(scene.doc.get(second)->block->find("visgroupid")->asInt().value()));
    tx.commit();
    const auto editor = fields(*scene.doc.get(scene.solid)->block->find("editor"));
    const auto before = *scene.doc.write();
    REQUIRE(scene.session.perform("clip.cycle_keep"));
    REQUIRE(scene.session.perform("clip.cycle_keep"));
    planeX(scene.session);
    key(scene.session, "Enter");
    REQUIRE(solids(scene.doc).size() == 2);
    for (const auto solid : solids(scene.doc)) {
        CHECK(scene.doc.get(solid)->parent == scene.doc.get(entity)->id);
        REQUIRE(scene.doc.get(solid)->block->find("editor"));
        CHECK(fields(*scene.doc.get(solid)->block->find("editor")) == editor);
    }
    REQUIRE(scene.session.perform("edit.undo"));
    CHECK(*scene.doc.write() == before);
}

TEST_CASE("clip non crossing and displacement solids stay untouched") {
    for (bool displacement : {false, true}) {
        Scene scene;
        if (displacement) {
            auto block = geometry::solidBlock(geometry_test::build(geometry_test::boxSides({-64, -64, -64}, {64, 64, 64})), {"fixture"});
            REQUIRE(block.find("side")->append(editor_test::block("dispinfo { power 2 }")));
            auto tx = scene.doc.begin("displacement fixture");
            tx.destroy(scene.solid);
            scene.solid = std::get<Handle<Solid>>(tx.create(block));
            tx.commit();
            scene.session.selection().select(scene.doc.get(scene.solid)->id);
        }
        const auto before = *scene.doc.write();
        const auto undo = scene.doc.undoCount();
        planeX(scene.session, displacement ? 0 : 128);
        CHECK(scene.session.preview().empty());
        if (displacement) CHECK(scene.session.activeToolObject().readout().warn);
        key(scene.session, "Enter");
        CHECK(scene.doc.undoCount() == undo);
        CHECK(*scene.doc.write() == before);
    }
}

TEST_CASE("clip per pane cleanup preserves other gestures and never notifies on forget") {
    Scene scene;
    planeX(scene.session, 0, true);
    place(scene.session, {16, -200, -200}, {-1, 0, 0}, true, 2);
    int notifications = 0;
    scene.session.observe([&] { ++notifications; });
    scene.session.forgetInputSource(1);
    CHECK(notifications == 0);
    CHECK_FALSE(scene.session.activeToolObject().owns(1));
    CHECK(scene.session.activeToolObject().owns(2));
    CHECK(scene.session.preview().empty());
    scene.session.setActiveTool("tool.select");
    CHECK_FALSE(scene.session.activeToolObject().owns(2));
}

TEST_CASE("clip placement uses vertex edge then surface grid and preset snap suspension") {
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) for (bool suspended : {false, true}) {
        Scene scene;
        scene.session.setPreset(*preset);
        scene.session.setGridSize(8);
        const std::array<DVec3, 3> raw{{{64, 62, 62}, {64, 1, 62}, {64, 19, 23}}};
        const std::array<DVec3, 3> snapped{{{64, 64, 64}, {64, 0, 64}, {64, 16, 24}}};
        const std::array names{"vertex", "midpoint", "grid"};
        for (std::size_t i = 0; i < raw.size(); ++i) {
            place(scene.session, raw[i], {-1, 0, 0}, true, 1, suspended);
            const auto overlay = scene.session.activeToolObject().overlay({}, 1);
            REQUIRE(overlay.worldPoints.size() == i + 1);
            checkPoint(overlay.worldPoints.back().position, suspended ? raw[i] : snapped[i]);
            CHECK(overlay.labels.back().text == (suspended ? "surface" : names[i]));
        }
    }
}

TEST_CASE("clip degenerate points cannot commit and cancelled gestures leave history intact") {
    Scene scene;
    const auto before = *scene.doc.write();
    const auto undo = scene.doc.undoCount();
    place(scene.session, {0, -200, -200});
    place(scene.session, {0, 0, -200});
    key(scene.session, "Enter");
    CHECK(scene.session.preview().empty());
    place(scene.session, {0, 200, -200});
    CHECK(scene.session.activeToolObject().readout().warn);
    key(scene.session, "Enter");
    CHECK(scene.doc.undoCount() == undo);
    CHECK(*scene.doc.write() == before);
    REQUIRE(scene.session.input(event(InputKind::Cancel), {}));
    CHECK_FALSE(scene.session.activeToolObject().owns(1));
    CHECK(scene.session.preview().empty());
}

TEST_CASE("clip orthographic empty space uses a camera facing fallback in all axes") {
    for (int axis = 0; axis < 3; ++axis) {
        Scene scene;
        DVec3 direction{0}, point{0};
        direction[axis] = -1;
        for (double offset : {-200.0, 200.0}) {
            point[(axis + 1) % 3] = offset;
            const auto view = viewAt(point, direction, true);
            REQUIRE_FALSE(scene.session.picks().first(view.cursorRay));
            auto press = event(InputKind::Press);
            press.alt = true;
            REQUIRE(scene.session.input(press, view));
            REQUIRE(scene.session.input(event(InputKind::Release), view));
        }
        REQUIRE(scene.session.preview().replacements().size() == 1);
        key(scene.session, "Enter");
        CHECK(geometry::solidGeometry(*scene.doc.get(scene.solid)->block)->poly.volume() == doctest::Approx(64 * 128 * 128));
    }
}

TEST_CASE("clip rounded source point data rewrites intact faces consistently") {
    Document doc{readCorpus(std::filesystem::path(APEX_FIXTURE_DIR) / "topology/clip_rounded_point_data.vmf")};
    const auto solid = solids(doc).front();
    const auto source = geometry::solidGeometry(*doc.get(solid)->block);
    REQUIRE(source);
    REQUIRE(source->usedPlaneFallback);
    checkClip(*doc.get(solid)->block);
}
