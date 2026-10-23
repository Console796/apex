#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "geometry/solid_block.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

using namespace editor_test;

namespace {
ToolView viewAt(DVec3 target, const SurfacePlane& plane, bool ortho = false) {
    ToolView view;
    view.cursorRay = {target + plane.normal * 1000.0, -plane.normal};
    view.size = {800, 600};
    view.cameraPosition = view.cursorRay.origin;
    view.cameraForward = -plane.normal;
    view.cameraUp = plane.vAxis;
    view.orthographic = ortho;
    view.orthoHeight = 600;
    return view;
}
const SurfacePlane floorPlane{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {}};
InputEvent event(InputKind kind, InputSource source = 1) {
    InputEvent e;
    e.kind = kind;
    e.source = source;
    if (kind == InputKind::Press || kind == InputKind::Release) e.button = PointerButton::Left;
    e.left = kind == InputKind::Press;
    return e;
}
void key(EditorSession& session, std::string name, InputSource source = 1) {
    auto e = event(InputKind::KeyDown, source);
    e.key = std::move(name);
    REQUIRE(session.input(e, {}));
}
void suspend(InputEvent& event, const Preset& preset) {
    event.alt = preset.name == "Hammer";
    event.ctrl = preset.name == "Blender";
}
void draw(EditorSession& session, const SurfacePlane& plane, DVec2 start, DVec2 end,
          bool ortho = false, bool suspended = false, bool square = false, InputSource source = 1) {
    const auto at = [&](DVec2 uv) { return viewAt(plane.origin + uv.x * plane.uAxis + uv.y * plane.vAxis, plane, ortho); };
    auto press = event(InputKind::Press, source);
    press.alt = suspended;
    press.shift = square;
    REQUIRE(session.input(press, at(start)));
    auto move = press;
    move.kind = InputKind::Move;
    REQUIRE(session.input(move, at(end)));
    CHECK(session.activeToolObject().overlay(at(end), source).worldSegments.size() == 4);
    auto release = press;
    release.kind = InputKind::Release;
    release.left = false;
    REQUIRE(session.input(release, at(end)));
}
geometry::ConvexPolyhedron selectedPoly(EditorSession& session) {
    const auto selected = selectedNodes(session.selection());
    REQUIRE(selected.size() == 1);
    const auto block = session.document()->get(*session.document()->handle(selected[0]))->block;
    auto solid = geometry::solidGeometry(*block);
    REQUIRE(solid);
    CHECK(solid->poly.validate().empty());
    CHECK(solid->poly.faces.size() == 6);
    return solid->poly;
}
void checkVertices(const geometry::ConvexPolyhedron& a, const geometry::ConvexPolyhedron& b) {
    CHECK(a.vertices.size() == b.vertices.size());
    for (const auto p : a.vertices)
        CHECK(std::ranges::any_of(b.vertices, [&](DVec3 q) { return glm::length(p - q) <= geometry::kPointMergeEps; }));
}
void checkKeys(EditorSession& session) {
    const auto selected = selectedNodes(session.selection());
    const auto& block = *session.document()->get(*session.document()->handle(selected[0]))->block;
    const std::vector<std::string> expected{"id", "plane", "point_data", "material", "uaxis", "vaxis",
                                            "rotation", "lightmapscale", "smoothing_groups"};
    for (const auto& side : block.children()) {
        if (side.key() != "side") continue;
        std::vector<std::string> keys;
        for (const auto& child : side.children()) keys.emplace_back(child.key());
        CHECK(keys == expected);
    }
}
struct Scene {
    Document doc{parsed()};
    EditorSession session;
    Scene() {
        session.attach(doc);
        session.setGridSize(8);
        REQUIRE(session.perform("tool.block"));
    }
};
SurfacePlane addSurface(Scene& scene, int kind) {
    if (kind == 0) {
        addBox(scene.doc, {-128, -128, -64}, {128, 128, 0});
        return floorPlane;
    }
    if (kind == 1) {
        addBox(scene.doc, {-64, -128, -128}, {0, 128, 128});
        return {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {}};
    }
    const double s = std::sqrt(0.5);
    auto poly = geometry_test::build(geometry_test::boxSides({-128, -128, -64}, {128, 128, 0}));
    for (auto& p : poly.vertices) p = {s * (p.x + p.z), p.y, s * (p.z - p.x)};
    for (auto& face : poly.faces) face.plane = *poly.derivePlane(face);
    auto tx = scene.doc.begin("slope fixture");
    tx.create(geometry::solidBlock(poly, {"fixture"}));
    tx.commit();
    return {{0, 0, 0}, {s, 0, s}, {s, 0, -s}, {0, 1, 0}, {}};
}
}

TEST_CASE("block draws floor wall slope and pinned planes in perspective and orthographic views") {
    for (bool ortho : {false, true}) for (int surface = 0; surface < 4; ++surface) {
        CAPTURE(ortho); CAPTURE(surface);
        Scene scene;
        auto plane = surface < 3 ? addSurface(scene, surface)
            : SurfacePlane{{256, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {}};
        if (surface == 3) {
            addBox(scene.doc, {-64, -128, -128}, {0, 128, 128});
            scene.session.pinAxisWorkPlane(Axis::X, plane.origin);
        }
        scene.session.setFallbackPlane(plane);
        const auto before = *scene.doc.write();
        const auto undo = scene.doc.undoCount();
        draw(scene.session, plane, {0, 0}, {32, 48}, ortho);
        key(scene.session, "1"); key(scene.session, "6");
        REQUIRE(scene.session.preview().ghosts().size() == 1);
        const auto ghost = scene.session.preview().ghosts()[0].poly;
        CHECK(scene.doc.undoCount() == undo);
        // A ghost cannot intercept a surface pick.
        if (surface < 3) {
            const auto hit = scene.session.picks().first(viewAt(plane.origin, plane).cursorRay);
            REQUIRE(hit);
            checkPoint(hit->point, plane.origin);
        }
        key(scene.session, "Enter");
        CHECK(scene.doc.undoCount() == undo + 1);
        CHECK(scene.session.preview().empty());
        const auto poly = selectedPoly(scene.session);
        checkVertices(ghost, poly);
        const auto bounds = poly.bounds();
        if (surface == 0) {
            checkPoint(bounds.min, {0, 0, 0}); checkPoint(bounds.max, {32, 48, 16});
        } else if (surface == 1) {
            checkPoint(bounds.min, {0, 0, 0}); checkPoint(bounds.max, {16, 32, 48});
        } else if (surface == 2) {
            const double s = std::sqrt(0.5);
            checkPoint(bounds.min, {0, 0, -32 * s}); checkPoint(bounds.max, {48 * s, 48, 16 * s});
        } else {
            checkPoint(bounds.min, {256, 0, 0}); checkPoint(bounds.max, {272, 32, 48});
        }
        checkKeys(scene.session);
        REQUIRE(scene.session.perform("edit.undo"));
        CHECK(*scene.doc.write() == before);
    }
}

TEST_CASE("block corners snap to the plane grid unless the preset suspend modifier is held") {
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) for (bool suspended : {false, true}) {
        Scene scene;
        scene.session.setPreset(*preset);
        auto press = event(InputKind::Press);
        press.alt = suspended && preset == &hammerPreset();
        press.ctrl = suspended && preset == &blenderPreset();
        REQUIRE(scene.session.input(press, viewAt({1.25, 2.5, 0}, floorPlane)));
        auto release = press; release.kind = InputKind::Release; release.left = false;
        REQUIRE(scene.session.input(release, viewAt({29.25, 43.5, 0}, floorPlane)));
        key(scene.session, "Enter");
        const auto bounds = selectedPoly(scene.session).bounds();
        checkPoint(bounds.min, suspended ? DVec3{1.25, 2.5, 0} : DVec3{0, 0, 0});
        checkPoint(bounds.max, suspended ? DVec3{29.25, 43.5, 64} : DVec3{32, 40, 64});
    }
}

TEST_CASE("block height follows a line with snapping and typed exact negative values") {
    Scene scene;
    draw(scene.session, floorPlane, {0, 0}, {32, 48});
    CHECK(scene.session.preview().ghosts()[0].poly.bounds().max.z == doctest::Approx(64));
    ToolView view;
    view.cursorRay = {{116, 24, 19.25}, {-1, 0, 0}};
    auto move = event(InputKind::Move);
    REQUIRE(scene.session.input(move, view));
    CHECK(scene.session.preview().ghosts()[0].poly.bounds().max.z == doctest::Approx(16));
    move.alt = true;
    REQUIRE(scene.session.input(move, view));
    CHECK(scene.session.preview().ghosts()[0].poly.bounds().max.z == doctest::Approx(19.25));
    for (auto name : {"1", "2", "Backspace", "7", ".", "5", "-"}) key(scene.session, name);
    CHECK(scene.session.activeToolObject().readout().status == "Height: -17.5");
    // Pointer movement cannot override typed height.
    REQUIRE(scene.session.input(move, view));
    const auto ghost = scene.session.preview().ghosts()[0].poly;
    REQUIRE(scene.session.input(event(InputKind::Press), view));
    const auto poly = selectedPoly(scene.session);
    checkPoint(poly.bounds().min, {0, 0, -17.5}); checkPoint(poly.bounds().max, {32, 48, 0});
    checkVertices(ghost, poly);
    const auto count = scene.doc.undoCount();
    scene.session.input(event(InputKind::Release), view);
    CHECK(scene.doc.undoCount() == count);
    draw(scene.session, floorPlane, {80, 0}, {112, 48});
    CHECK(scene.session.preview().ghosts()[0].poly.bounds().min.z == doctest::Approx(-17.5));
}

TEST_CASE("block shift square defaults height to the base side") {
    Scene scene;
    draw(scene.session, floorPlane, {0, 0}, {32, 48}, false, false, true);
    key(scene.session, "Enter");
    checkPoint(selectedPoly(scene.session).bounds().max, {48, 48, 48});
}

TEST_CASE("block back-step cancellation and degenerate geometry leave history unchanged") {
    Scene scene;
    const auto bytes = *scene.doc.write();
    const auto count = scene.doc.undoCount();
    draw(scene.session, floorPlane, {0, 0}, {0, 0});
    CHECK(scene.session.preview().empty());
    key(scene.session, "Escape");
    draw(scene.session, floorPlane, {0, 0}, {32, 48});
    key(scene.session, "0");
    key(scene.session, "Enter");
    CHECK(scene.session.activeToolObject().owns(1));
    CHECK(scene.session.preview().empty());
    key(scene.session, "Backspace"); key(scene.session, "-");
    key(scene.session, "Enter"); // Incomplete number cannot commit either.
    CHECK(scene.session.preview().empty());
    auto right = event(InputKind::Press); right.button = PointerButton::Right; right.navigating = true;
    REQUIRE(scene.session.input(right, {}));
    CHECK(scene.session.preview().empty());
    CHECK_FALSE(scene.session.activeToolObject().owns(1));
    CHECK(scene.session.activeToolObject().overlay({}, 1).worldSegments.empty());
    REQUIRE(scene.session.input(right, {}));
    draw(scene.session, floorPlane, {0, 0}, {32, 48});
    key(scene.session, "Escape");
    auto press = event(InputKind::Press);
    scene.session.input(press, viewAt({0, 0, 0}, floorPlane));
    key(scene.session, "Escape");
    CHECK(scene.doc.undoCount() == count);
    CHECK(*scene.doc.write() == bytes);
}

TEST_CASE("block gesture state and cleanup belong to each input source") {
    for (int cleanup = 0; cleanup < 3; ++cleanup) {
        Scene scene;
        draw(scene.session, floorPlane, {0, 0}, {32, 48});
        draw(scene.session, floorPlane, {80, 0}, {112, 48}, false, false, false, 2);
        REQUIRE(scene.session.preview().ghosts().size() == 2);
        if (cleanup == 0) scene.session.forgetInputSource(1);
        if (cleanup == 1) scene.session.input(event(InputKind::Cancel), {});
        if (cleanup == 2) {
            scene.session.setActiveTool("tool.select");
            scene.session.setActiveTool("tool.block");
            CHECK(scene.session.preview().empty());
            continue;
        }
        REQUIRE(scene.session.preview().ghosts().size() == 1);
        CHECK(scene.session.activeToolObject().owns(2));
        CHECK_FALSE(scene.session.activeToolObject().owns(1));
        key(scene.session, "Enter", 2);
        checkPoint(selectedPoly(scene.session).bounds().min, {80, 0, 0});
    }
}

TEST_CASE("block leave cancels base drag but preserves height stage for typing") {
    Scene scene;
    auto press = event(InputKind::Press);
    REQUIRE(scene.session.input(press, viewAt({0, 0, 0}, floorPlane)));
    REQUIRE(scene.session.activeToolObject().owns(1));
    REQUIRE(scene.session.input(event(InputKind::Leave), {}));
    CHECK_FALSE(scene.session.activeToolObject().owns(1));
    CHECK(scene.session.preview().empty());

    draw(scene.session, floorPlane, {0, 0}, {32, 48});
    REQUIRE(scene.session.preview().ghosts().size() == 1);
    const auto before = scene.session.preview().ghosts()[0].poly;
    REQUIRE(scene.session.input(event(InputKind::Leave), {}));
    CHECK(scene.session.activeToolObject().owns(1));
    REQUIRE(scene.session.preview().ghosts().size() == 1);
    checkVertices(before, scene.session.preview().ghosts()[0].poly);
    key(scene.session, "2"); key(scene.session, "4"); key(scene.session, "Enter");
    checkPoint(selectedPoly(scene.session).bounds().max, {32, 48, 24});
}

TEST_CASE("block survives pane CancelPointer sequence and commits once") {
    Scene scene;
    const auto history = scene.doc.undoCount();
    auto press = event(InputKind::Press);
    REQUIRE(scene.session.input(press, viewAt({0, 0, 0}, floorPlane)));
    CHECK_FALSE(scene.session.input(event(InputKind::CancelPointer), {}));
    CHECK(scene.session.activeToolObject().owns(1));

    auto move = event(InputKind::Move);
    move.left = true;
    REQUIRE(scene.session.input(move, viewAt({32, 48, 0}, floorPlane)));
    auto release = event(InputKind::Release);
    REQUIRE(scene.session.input(release, viewAt({32, 48, 0}, floorPlane)));
    ToolView height;
    height.cursorRay = {{100, 24, 19}, {-1, 0, 0}};
    REQUIRE(scene.session.input(event(InputKind::Move), height));
    REQUIRE(scene.session.input(event(InputKind::Press), height));
    CHECK(scene.doc.undoCount() == history + 1);
    checkPoint(selectedPoly(scene.session).bounds().max, {32, 48, 16});
}

TEST_CASE("block CancelPointer keeps height stage and missing release finishes base") {
    Scene scene;
    auto press = event(InputKind::Press);
    REQUIRE(scene.session.input(press, viewAt({0, 0, 0}, floorPlane)));
    auto lostRelease = event(InputKind::Move); // Pane observes that left is no longer held.
    REQUIRE(scene.session.input(lostRelease, viewAt({32, 48, 0}, floorPlane)));
    CHECK(scene.session.activeToolObject().owns(1));
    REQUIRE(scene.session.preview().ghosts().size() == 1);
    const auto before = scene.session.preview().ghosts()[0].poly;
    CHECK_FALSE(scene.session.input(event(InputKind::CancelPointer), {}));
    CHECK(scene.session.activeToolObject().owns(1));
    REQUIRE(scene.session.preview().ghosts().size() == 1);
    checkVertices(before, scene.session.preview().ghosts()[0].poly);
    key(scene.session, "Enter");
    selectedPoly(scene.session);
}

TEST_CASE("block applies session material and Face versus World UVs on slopes") {
    std::vector<geometry::ConvexPolyhedron> results;
    for (auto alignment : {geometry::UvAlignment::Face, geometry::UvAlignment::World}) {
        Scene scene;
        CHECK(scene.session.newSolidStyle().material == "dev/dev_measuregeneric01");
        CHECK(scene.session.newSolidStyle().alignment == geometry::UvAlignment::Face);
        auto plane = addSurface(scene, 2);
        scene.session.setNewSolidMaterial("custom/block");
        scene.session.setNewSolidUvAlignment(alignment);
        draw(scene.session, plane, {0, 0}, {32, 48});
        key(scene.session, "Enter");
        results.push_back(selectedPoly(scene.session));
        for (const auto& face : results.back().faces) {
            CHECK(face.material == "custom/block");
            CHECK(face.uv.scaleU == doctest::Approx(0.25));
            if (alignment == geometry::UvAlignment::Face)
                CHECK(glm::dot(face.uv.uaxis, face.plane.normal) == doctest::Approx(0).epsilon(1e-6));
        }
    }
    bool differs = false;
    for (std::size_t i = 0; i < results[0].faces.size(); ++i)
        differs |= glm::length(results[0].faces[i].uv.uaxis - results[1].faces[i].uv.uaxis) > 0.1;
    CHECK(differs);
}

TEST_CASE("block optional VMF export contains floor wall and slope draws") {
    Document output{parsed()};
    for (int surface = 0; surface < 3; ++surface) {
        Scene scene;
        const auto plane = addSurface(scene, surface);
        draw(scene.session, plane, {0, 0}, {32, 48});
        key(scene.session, "1"); key(scene.session, "6"); key(scene.session, "Enter");
        selectedPoly(scene.session);
        auto tx = output.begin("export drawn blocks");
        for (const auto handle : nodes(scene.doc)) {
            if (!std::holds_alternative<Handle<Solid>>(handle)) continue;
            auto solid = geometry::solidGeometry(*scene.doc.get(handle)->block);
            REQUIRE(solid);
            translate(*solid, {0, surface * 512.0, 0});
            tx.create(geometry::solidBlock(solid->poly, scene.session.newSolidStyle()));
        }
        tx.commit();
    }
    if (const char* path = std::getenv("APEX_BLOCK_VMF_OUT")) {
        const auto bytes = output.write();
        REQUIRE(bytes);
        std::ofstream out(path);
        REQUIRE(out);
        out << *bytes;
        REQUIRE(out.good());
    }
}

TEST_CASE("block orthographic fallback faces the camera without a supplied plane") {
    Scene scene;
    // A visible surface must not replace the camera-facing orthographic fallback.
    addBox(scene.doc, {-64, -128, -128}, {-32, 128, 128});
    const SurfacePlane wall{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {}};
    draw(scene.session, wall, {0, 0}, {32, 48}, true);
    key(scene.session, "1"); key(scene.session, "6"); key(scene.session, "Enter");
    const auto bounds = selectedPoly(scene.session).bounds();
    checkPoint(bounds.min, {0, 0, 0}); checkPoint(bounds.max, {16, 32, 48});
}

TEST_CASE("block empty front and side orthographic views use their camera-facing planes") {
    const std::array cases{
        std::pair{SurfacePlane{{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {0, 0, -1}, {}},
                  AABB{{0, 0, -48}, {32, 16, 0}}},
        std::pair{SurfacePlane{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {}},
                  AABB{{0, 0, 0}, {16, 32, 48}}},
    };
    for (const auto& [plane, expected] : cases) {
        Scene scene;
        CHECK(scene.session.picks().size() == 0);
        draw(scene.session, plane, {0, 0}, {32, 48}, true);
        key(scene.session, "1"); key(scene.session, "6"); key(scene.session, "Enter");
        const auto bounds = selectedPoly(scene.session).bounds();
        checkPoint(bounds.min, expected.min);
        checkPoint(bounds.max, expected.max);
    }
}

TEST_CASE("block selects the created solid when started in face selection mode") {
    Scene scene;
    scene.session.selection().setMode(SelectionMode::Faces);
    draw(scene.session, floorPlane, {0, 0}, {32, 48});
    key(scene.session, "Enter");
    selectedPoly(scene.session);
    CHECK(scene.session.selection().mode() == SelectionMode::Solids);
    REQUIRE(scene.session.perform("edit.undo"));
    CHECK(scene.session.selection().mode() == SelectionMode::Faces);
}

TEST_CASE("block respects each preset snap suspend modifier for corners and height") {
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) {
        Scene scene;
        scene.session.setPreset(*preset);
        auto press = event(InputKind::Press);
        suspend(press, *preset);
        REQUIRE(scene.session.input(press, viewAt({1.25, 2.5, 0}, floorPlane)));
        auto move = press; move.kind = InputKind::Move;
        REQUIRE(scene.session.input(move, viewAt({29.25, 43.5, 0}, floorPlane)));
        auto release = press; release.kind = InputKind::Release; release.left = false;
        REQUIRE(scene.session.input(release, viewAt({29.25, 43.5, 0}, floorPlane)));
        ToolView heightView;
        heightView.cursorRay = {{100, 23, 19.25}, {-1, 0, 0}};
        move = event(InputKind::Move);
        suspend(move, *preset);
        REQUIRE(scene.session.input(move, heightView));
        key(scene.session, "Enter");
        const auto bounds = selectedPoly(scene.session).bounds();
        checkPoint(bounds.min, {1.25, 2.5, 0});
        checkPoint(bounds.max, {29.25, 43.5, 19.25});
    }
}

TEST_CASE("block drag corner uses shared feature snapping and suspend bypasses it") {
    for (bool suspended : {false, true}) {
        Scene scene;
        addBox(scene.doc, {40, 40, -16}, {50, 50, 0});
        auto press = event(InputKind::Press);
        press.alt = suspended;
        REQUIRE(scene.session.input(press, viewAt({0, 0, 0}, floorPlane, true)));
        auto move = press; move.kind = InputKind::Move;
        REQUIRE(scene.session.input(move, viewAt({48, 48, 0}, floorPlane, true)));
        const auto overlay = scene.session.activeToolObject().overlay({}, 1);
        REQUIRE(overlay.worldPoints.size() == 1);
        REQUIRE(overlay.labels.size() == 1);
        checkPoint(overlay.worldPoints[0].position, suspended ? DVec3{48, 48, 0} : DVec3{50, 50, 0});
        CHECK(overlay.labels[0].text == (suspended ? "surface" : "vertex"));
        auto release = press; release.kind = InputKind::Release; release.left = false;
        REQUIRE(scene.session.input(release, viewAt({48, 48, 0}, floorPlane, true)));
        key(scene.session, "Enter");
        const auto bounds = selectedPoly(scene.session).bounds();
        checkPoint(bounds.max, suspended ? DVec3{48, 48, 64} : DVec3{50, 50, 64});
    }
}

TEST_CASE("block parallel orthographic rays choose height by vertical screen motion") {
    Scene scene;
    draw(scene.session, floorPlane, {0, 0}, {32, 48}, true);
    auto view = viewAt({32, 48, 0}, floorPlane, true);
    auto move = event(InputKind::Move);
    move.cursor.y = -19.25; // One world unit per pixel; default height is 64.
    REQUIRE(scene.session.input(move, view));
    CHECK(scene.session.preview().ghosts()[0].poly.bounds().max.z == doctest::Approx(80));
    move.alt = true;
    REQUIRE(scene.session.input(move, view));
    CHECK(scene.session.preview().ghosts()[0].poly.bounds().max.z == doctest::Approx(83.25));
    move.cursor.y = 80.5;
    REQUIRE(scene.session.input(move, view));
    const auto ghost = scene.session.preview().ghosts()[0].poly;
    key(scene.session, "Enter");
    const auto poly = selectedPoly(scene.session);
    checkPoint(poly.bounds().min, {0, 0, -16.5});
    checkPoint(poly.bounds().max, {32, 48, 0});
    checkVertices(ghost, poly);
}
