// Apex-authored vertex tool test geometry (MIT)
#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/vertex_tool.hpp"
#include "geometry/solid_block.hpp"

#include <algorithm>

using namespace editor_test;

namespace {
struct Scene {
    Document doc{parsed()};
    Handle<Solid> solid;
    EditorSession session;
    NodeId node = 0;
    Scene() {
        solid = std::get<Handle<Solid>>(addBox(doc, {-64,-64,-64}, {64,64,64}));
        node = doc.get(solid)->id;
        session.attach(doc);
        session.setPreset(blenderPreset());
        session.setGridSize(8);
        session.selection().setMode(SelectionMode::Solids);
        session.selection().select(node);
        REQUIRE(session.perform("tool.vertex"));
    }
    VertexTool& tool() { return static_cast<VertexTool&>(session.activeToolObject()); }
};
ToolView viewAt(DVec3 target, DVec3 direction = {-1,0,0}) {
    ToolView view;
    view.cursorRay = {target - direction * 1000.0, direction};
    view.cameraPosition = view.cursorRay.origin;
    view.cameraForward = direction;
    view.cameraUp = std::abs(direction.z) > 0.9 ? DVec3{0,1,0} : DVec3{0,0,1};
    view.size = {800,600};
    view.orthographic = true;
    view.orthoHeight = 600;
    return view;
}
InputEvent event(InputKind kind, std::string key = {}) {
    InputEvent e;
    e.kind = kind;
    e.source = 1;
    e.key = std::move(key);
    if (kind == InputKind::Press || kind == InputKind::Release) e.button = PointerButton::Left;
    e.left = kind == InputKind::Press;
    return e;
}
void selectVertex(Scene& scene, DVec3 point, bool add = false) {
    auto e = event(InputKind::Press);
    e.shift = add;
    REQUIRE(scene.session.input(e, viewAt(point)));
}
void modalKey(Scene& scene, std::string key) {
    auto e = event(InputKind::KeyDown, std::move(key));
    REQUIRE(scene.session.input(e, {}));
}
void startGrab(Scene& scene, DVec3 at) {
    auto e = event(InputKind::KeyDown, "G");
    e.chord = KeyChord{"G"};
    REQUIRE(scene.session.input(e, viewAt(at)));
}
void sameVertices(const geometry::ConvexPolyhedron& a, const geometry::ConvexPolyhedron& b) {
    CHECK(a.vertices.size() == b.vertices.size());
    for (const auto point : a.vertices)
        CHECK(std::ranges::any_of(b.vertices, [&](DVec3 other) {
            return glm::length(point-other) <= geometry::kPointMergeEps;
        }));
}
}

TEST_CASE("vertex tool commits the same hull it previews and undo restores bytes") {
    Scene scene;
    selectVertex(scene, {64,64,64});
    REQUIRE(scene.tool().elements().size() == 1);
    const auto before = *scene.doc.write();
    const auto history = scene.doc.undoCount();
    startGrab(scene, {64,64,64});
    modalKey(scene, "Z"); modalKey(scene, "1"); modalKey(scene, "6");
    REQUIRE(scene.session.preview().replacements().contains(scene.node));
    const auto preview = scene.session.preview().replacements().at(scene.node);
    modalKey(scene, "Enter");
    CHECK(scene.doc.undoCount() == history + 1);
    const auto committed = geometry::solidGeometry(*scene.doc.get(scene.solid)->block);
    REQUIRE(committed);
    sameVertices(preview, committed->poly);
    REQUIRE(scene.session.perform("edit.undo"));
    CHECK(*scene.doc.write() == before);
}

TEST_CASE("vertex tool marks an absorbed vertex in the live preview status") {
    Scene scene;
    selectVertex(scene, {64,64,64});
    startGrab(scene, {64,64,64});
    auto move = event(InputKind::Move);
    move.ctrl = true; // Blender's snap-suspend modifier.
    REQUIRE(scene.session.input(move, viewAt({64,0,0})));
    const auto readout = scene.tool().readout();
    INFO("move status: " << readout.status);
    CHECK(readout.status.find("absorbed") != std::string::npos);
    CHECK_FALSE(scene.session.preview().empty());
    modalKey(scene, "Escape");
}

TEST_CASE("vertex tool requires confirmation before welding coincident vertices") {
    Scene scene;
    selectVertex(scene, {64,64,64});
    const auto before = *scene.doc.write();
    const auto history = scene.doc.undoCount();
    startGrab(scene, {64,64,64});
    modalKey(scene, "X"); modalKey(scene, "-");
    for (const auto* digit : {"1","2","8"}) modalKey(scene, digit);
    CHECK(scene.tool().readout().status.find("merge") != std::string::npos);
    modalKey(scene, "Enter");
    CHECK(scene.tool().readout().status.find("confirms") != std::string::npos);
    CHECK(*scene.doc.write() == before);
    CHECK(scene.doc.undoCount() == history);
    modalKey(scene, "Enter");
    CHECK(scene.doc.undoCount() == history + 1);
}

TEST_CASE("vertex tool refuses a collapsed face move without changing document bytes") {
    Scene scene;
    REQUIRE(scene.session.perform("vertex.mode.face"));
    REQUIRE(scene.session.input(event(InputKind::Press), viewAt({0,0,64}, {0,0,-1})));
    REQUIRE(scene.session.input(event(InputKind::Release), viewAt({0,0,64}, {0,0,-1})));
    const auto before = *scene.doc.write();
    const auto history = scene.doc.undoCount();
    startGrab(scene, {0,0,64});
    modalKey(scene, "Z"); modalKey(scene, "-");
    for (const auto* digit : {"1","2","8"}) modalKey(scene, digit);
    INFO("pre-commit status: " << scene.tool().readout().status);
    if (scene.session.preview().replacements().contains(scene.node)) {
        const auto bounds = scene.session.preview().replacements().at(scene.node).bounds();
        INFO("preview z: " << bounds.min.z << " .. " << bounds.max.z);
    }
    CHECK(scene.tool().readout().warn);
    CHECK(scene.session.preview().empty());
    CHECK(*scene.doc.write() == before);
    CHECK(scene.doc.undoCount() == history);
    modalKey(scene, "Enter");
    CHECK(*scene.doc.write() == before);
    CHECK(scene.doc.undoCount() == history);
}

TEST_CASE("vertex tool typed move matches an axis gizmo drag") {
    Scene modal;
    selectVertex(modal, {64,64,64});
    startGrab(modal, {64,64,64});
    modalKey(modal, "Z"); modalKey(modal, "1"); modalKey(modal, "6");
    const auto modalPreview = modal.session.preview().replacements().at(modal.node);
    modalKey(modal, "Escape");

    Scene gizmo;
    selectVertex(gizmo, {64,64,64});
    const auto solidGeometry = geometry::solidGeometry(*gizmo.doc.get(gizmo.solid)->block);
    REQUIRE(solidGeometry);
    const auto vertexIndex = gizmo.tool().elements().front().a;
    const auto picked = solidGeometry->poly.vertices[vertexIndex];
    INFO("selected vertex: " << picked.x << "," << picked.y << "," << picked.z);
    const auto view = viewAt(picked);
    REQUIRE(gizmo.tool().beginGizmoDrag(GizmoHandle::Z,
        gizmoViewFor(view, gizmo.tool().gizmoContext()->pivot), view.cursorRay));
    auto moved = view;
    moved.cursorRay = {picked + DVec3{1000,0,16}, {-1,0,0}};
    REQUIRE(gizmo.tool().updateGizmoDrag(moved.cursorRay));
    REQUIRE(gizmo.session.preview().replacements().contains(gizmo.node));
    INFO("typed bounds: " << modalPreview.bounds().min.z << " .. " << modalPreview.bounds().max.z);
    INFO("gizmo status: " << gizmo.tool().readout().status);
    INFO("gizmo bounds: " << gizmo.session.preview().replacements().at(gizmo.node).bounds().min.z
         << " .. " << gizmo.session.preview().replacements().at(gizmo.node).bounds().max.z);
    sameVertices(modalPreview, gizmo.session.preview().replacements().at(gizmo.node));
}

TEST_CASE("vertex element mode switches between vertex, edge and face handles") {
    Scene scene;
    CHECK(scene.tool().elementMode() == ElementMode::Vertex);
    REQUIRE(scene.session.perform("vertex.mode.edge"));
    CHECK(scene.tool().elementMode() == ElementMode::Edge);
    auto edgeClick = event(InputKind::Press);
    REQUIRE(scene.session.input(edgeClick, viewAt({64,0,64})));
    REQUIRE(scene.tool().elements().size() == 1);
    CHECK(scene.tool().elements().front().kind == ElementKind::Edge);
    REQUIRE(scene.session.perform("vertex.mode.face"));
    CHECK(scene.tool().elementMode() == ElementMode::Face);
    auto faceClick = event(InputKind::Press);
    REQUIRE(scene.session.input(faceClick, viewAt({0,0,64}, {0,0,-1})));
    REQUIRE(scene.tool().elements().size() == 1);
    CHECK(scene.tool().elements().front().kind == ElementKind::Face);
}

TEST_CASE("vertex tool marquee selects element handles in the active mode") {
    Scene scene;
    auto press = event(InputKind::Press);
    press.cursor = {0,0};
    REQUIRE(scene.session.input(press, viewAt({300,300,300})));
    auto move = event(InputKind::Move);
    move.left = true;
    move.cursor = {800,600};
    REQUIRE(scene.session.input(move, viewAt({0,0,0})));
    auto release = event(InputKind::Release);
    release.cursor = move.cursor;
    REQUIRE(scene.session.input(release, viewAt({0,0,0})));
    CHECK(scene.tool().elements().size() == 8);
    CHECK(std::ranges::all_of(scene.tool().elements(), [](const auto& element) {
        return element.kind == ElementKind::Vertex;
    }));
}

namespace {
// Press the top face from above, then drag in a side view whose ray crosses the face normal at z.
void pushTopFace(Scene& scene, double z, bool suspend = false, bool release = true) {
    REQUIRE(scene.session.perform("vertex.mode.face"));
    REQUIRE(scene.session.input(event(InputKind::Press), viewAt({16,16,64}, {0,0,-1})));
    auto move = event(InputKind::Move);
    move.ctrl = suspend; // Blender's snap-suspend modifier.
    REQUIRE(scene.session.input(move, viewAt({0,0,z})));
    if (release) REQUIRE(scene.session.input(event(InputKind::Release), viewAt({0,0,z})));
}
}

TEST_CASE("vertex tool face mode drags a pressed face along its normal") {
    Scene scene;
    const auto before = *scene.doc.write();
    const auto history = scene.doc.undoCount();
    const auto source = geometry::solidGeometry(*scene.doc.get(scene.solid)->block);
    REQUIRE(source);
    pushTopFace(scene, 88);
    CHECK(scene.doc.undoCount() == history + 1);
    const auto pulled = geometry::solidGeometry(*scene.doc.get(scene.solid)->block);
    REQUIRE(pulled);
    CHECK(pulled->poly.bounds().max.z == doctest::Approx(88));
    CHECK(pulled->poly.bounds().min.z == doctest::Approx(-64));
    CHECK(pulled->poly.bounds().max.x == doctest::Approx(64));
    REQUIRE(pulled->poly.faces.size() == 6);
    for (const auto& face : source->poly.faces)
        CHECK(std::ranges::find(pulled->poly.faces, face.sideId, &geometry::Face::sideId) != pulled->poly.faces.end());

    pushTopFace(scene, 40);
    const auto pushed = geometry::solidGeometry(*scene.doc.get(scene.solid)->block);
    REQUIRE(pushed);
    CHECK(pushed->poly.bounds().max.z == doctest::Approx(40));
    REQUIRE(scene.session.perform("edit.undo"));
    REQUIRE(scene.session.perform("edit.undo"));
    CHECK(*scene.doc.write() == before);
}

TEST_CASE("vertex tool face push snaps to the grid unless suspended") {
    Scene scene;
    pushTopFace(scene, 91, false, false);
    CHECK(scene.tool().readout().status.find("Push/pull 24 units") != std::string::npos);
    CHECK(scene.tool().overlay(viewAt({0,0,91}), 1).segments.empty()); // No gizmo while pushing.
    CHECK(scene.session.preview().replacements().at(scene.node).bounds().max.z == doctest::Approx(88));
    auto move = event(InputKind::Move);
    move.ctrl = true;
    REQUIRE(scene.session.input(move, viewAt({0,0,91})));
    CHECK(scene.session.preview().replacements().at(scene.node).bounds().max.z == doctest::Approx(91));
    modalKey(scene, "Escape");
}

TEST_CASE("vertex tool face click without a drag only selects the face") {
    Scene scene;
    const auto before = *scene.doc.write();
    const auto history = scene.doc.undoCount();
    REQUIRE(scene.session.perform("vertex.mode.face"));
    REQUIRE(scene.session.input(event(InputKind::Press), viewAt({16,16,64}, {0,0,-1})));
    REQUIRE(scene.session.input(event(InputKind::Release), viewAt({16,16,64}, {0,0,-1})));
    CHECK(scene.doc.undoCount() == history);
    CHECK(*scene.doc.write() == before);
    CHECK(scene.session.preview().empty());
    REQUIRE(scene.tool().elements().size() == 1);
    CHECK(scene.tool().elements().front().kind == ElementKind::Face);
    CHECK_FALSE(scene.tool().overlay(viewAt({0,0,200}), 1).segments.empty()); // Gizmo back once released.
}

TEST_CASE("vertex tool face press under the gizmo pushes the face, not the gizmo") {
    Scene scene;
    REQUIRE(scene.session.perform("vertex.mode.face"));
    const auto top = viewAt({0,0,64}, {0,0,-1});
    REQUIRE(scene.session.input(event(InputKind::Press), top));
    REQUIRE(scene.session.input(event(InputKind::Release), top));
    REQUIRE_FALSE(scene.tool().overlay(top, 1).segments.empty()); // Gizmo sits on the face centre.
    REQUIRE(scene.session.input(event(InputKind::Press), top));
    REQUIRE(scene.session.input(event(InputKind::Move), viewAt({0,0,88})));
    CHECK(scene.tool().readout().status.find("Push/pull 24 units") != std::string::npos);
    CHECK_FALSE(scene.tool().dragged(1));
    CHECK(scene.tool().overlay(viewAt({0,0,88}), 1).segments.empty());
    modalKey(scene, "Escape");
}

TEST_CASE("vertex tool face mode highlights the hovered face and hides edge midpoints") {
    Scene scene;
    REQUIRE(scene.session.perform("vertex.mode.face"));
    const auto view = viewAt({40,-30,64}, {0,0,-1});
    CHECK(scene.tool().overlay(view, 1).worldTriangles.empty());
    REQUIRE(scene.session.input(event(InputKind::Move), view));
    const auto overlay = scene.tool().overlay(view, 1);
    CHECK(overlay.worldTriangles.size() == 2); // The top quad, fanned.
    for (const auto& triangle : overlay.worldTriangles)
        for (const auto point : {triangle.a, triangle.b, triangle.c}) CHECK(point.z == doctest::Approx(64));
    CHECK(overlay.worldPoints.size() == 8 + 6); // Corners and face centres; no edge midpoints.
    auto leave = event(InputKind::Leave);
    scene.session.input(leave, view);
    CHECK(scene.tool().overlay(view, 1).worldTriangles.empty());
}

TEST_CASE("vertex tool draws with the session's overlay palette") {
    Scene scene;
    OverlayPalette palette;
    palette.vertexHandle = {0.0f, 1.0f, 0.0f, 1.0f};
    palette.faceHover = {1.0f, 0.0f, 1.0f, 1.0f};
    scene.session.setOverlayPalette(palette);
    const auto view = viewAt({40,-30,64}, {0,0,-1});
    auto overlay = scene.tool().overlay(view, 1);
    CHECK(std::ranges::count_if(overlay.worldPoints, [&](const auto& p) { return p.colour == palette.vertexHandle; }) == 8);
    REQUIRE(scene.session.perform("vertex.mode.face"));
    REQUIRE(scene.session.input(event(InputKind::Move), view));
    overlay = scene.tool().overlay(view, 1);
    REQUIRE_FALSE(overlay.worldTriangles.empty());
    CHECK(Vec3(overlay.worldTriangles.front().colour) == Vec3(palette.faceHover));
    CHECK(overlay.worldTriangles.front().colour.a < 1.0f);
}

TEST_CASE("vertex topology refuses displacement solids with a reason") {
    Scene scene;
    auto block = geometry::solidBlock(geometry_test::build(geometry_test::boxSides({-64,-64,-64}, {64,64,64})), {"apex/displacement"});
    REQUIRE(block.find("side")->append(editor_test::block("dispinfo { power 2 }")));
    auto tx = scene.doc.begin("displacement fixture");
    tx.destroy(scene.solid);
    scene.solid = std::get<Handle<Solid>>(tx.create(std::move(block)));
    tx.commit();
    scene.session.selection().select(scene.doc.get(scene.solid)->id);
    const auto before = *scene.doc.write();
    const auto count = scene.doc.undoCount();
    selectVertex(scene, {64,64,64});
    CHECK(scene.tool().elements().empty());
    CHECK(scene.tool().readout().status.find("Displacement") != std::string::npos);
    CHECK(scene.session.preview().empty());
    CHECK(*scene.doc.write() == before);
    CHECK(scene.doc.undoCount() == count);
}
