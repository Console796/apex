// Plan 0.8 D5 (sub-milestone 0.8.3): each topology tool's rule on a displacement brush, as a test
// (refuse, allow, re-base). The brush is solid 637 of test1_disp.vmf, whose top side 355 is displaced.
#include "fixtures.hpp"
#include "document/displacement.hpp"
#include "editor/editor_session.hpp"
#include "editor/topology_ops.hpp"
#include "editor/transform.hpp"
#include "editor/vertex_tool.hpp"
#include "geometry/displacement.hpp"
#include "geometry/displacement_rebase.hpp"

#include <fstream>
#include <sstream>

using namespace editor_test;

namespace {

constexpr SideId kSide = 355; // The displaced top (z = 64) of solid 637; the box is x, y in +-96 about (0, -1536), z 0..64.

std::string fixture() {
    std::ifstream in(std::string(APEX_FIXTURE_DIR) + "/vmf/disp/test1_disp.vmf", std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

Handle<Solid> solidOf(const Document& doc, SideId side) {
    const auto location = doc.side(side);
    REQUIRE(location);
    return location->solid;
}

geometry::ConvexPolyhedron polyOf(const Document& doc, Handle<Solid> solid) {
    const auto geometry = geometry::solidGeometry(*doc.get(solid)->block);
    REQUIRE(geometry);
    return geometry->poly;
}

std::array<DVec3, 4> cornersOf(const Document& doc, SideId side) {
    const auto poly = polyOf(doc, solidOf(doc, side));
    const auto face = std::ranges::find(poly.faces, static_cast<std::uint32_t>(side), &geometry::Face::sideId);
    REQUIRE(face != poly.faces.end());
    const auto corners = geometry::displacementCorners(poly, *face);
    REQUIRE(corners);
    return *corners;
}

geometry::Displacement dispOf(const Document& doc, SideId side) {
    const auto disp = document::displacement(doc, side);
    REQUIRE(disp);
    REQUIRE(disp->has_value());
    return **disp;
}

geometry::DisplacementMesh meshOf(const std::array<DVec3, 4>& corners, const geometry::Displacement& disp) {
    auto mesh = geometry::displacementMesh(corners, disp);
    REQUIRE(mesh);
    return std::move(*mesh);
}

std::string text(const kv1::Node& node) {
    std::string out = std::string(node.rawKey()) + " " + std::string(node.rawValue().value_or("{")) + "\n";
    for (const auto& child : node.children()) out += text(child);
    return out;
}

// The side's dispinfo block exactly as written.
std::string dispinfoText(const Document& doc, SideId side) {
    const auto location = doc.side(side);
    REQUIRE(location);
    const auto sides = doc.get(location->solid)->block->findAll("side");
    REQUIRE(location->faceIndex < sides.size());
    const auto* dispinfo = sides[location->faceIndex]->find("dispinfo");
    REQUIRE(dispinfo);
    return text(*dispinfo);
}

void sameCorners(const std::array<DVec3, 4>& a, const std::array<DVec3, 4>& b) {
    for (const auto corner : a)
        CHECK(std::ranges::any_of(b, [&](DVec3 other) { return glm::distance(corner, other) < 1e-9; }));
}

// --- vertex tool ---------------------------------------------------------------------------

struct VertexScene {
    Document doc{parsed(fixture())};
    EditorSession session;
    Handle<Solid> solid;
    VertexScene() {
        solid = solidOf(doc, kSide);
        session.attach(doc);
        session.setPreset(blenderPreset());
        session.setGridSize(8);
        session.selection().setMode(SelectionMode::Solids);
        session.selection().select(doc.get(solid)->id);
        REQUIRE(session.perform("tool.vertex"));
    }
    VertexTool& tool() { return static_cast<VertexTool&>(session.activeToolObject()); }
};

ToolView viewAt(DVec3 target, DVec3 direction) {
    ToolView view;
    view.cursorRay = {target - direction * 1000.0, direction};
    view.cameraPosition = view.cursorRay.origin;
    view.cameraForward = direction;
    view.cameraUp = std::abs(direction.z) > 0.9 ? DVec3{0, 1, 0} : DVec3{0, 0, 1};
    view.size = {800, 600};
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
// Clicks a brush vertex. The brush's flat sides are hidden, so the tool hits its vertices in screen
// space; the one nearest the camera wins where two project to the same pixel.
void click(VertexScene& scene, DVec3 point, DVec3 direction, bool add) {
    auto e = event(InputKind::Press);
    e.shift = add;
    REQUIRE(scene.session.input(e, viewAt(point, direction)));
}
void keys(VertexScene& scene, std::initializer_list<const char*> names) {
    for (const auto* name : names) REQUIRE(scene.session.input(event(InputKind::KeyDown, name), {}));
}
void grab(VertexScene& scene, DVec3 at, DVec3 direction) {
    auto e = event(InputKind::KeyDown, "G");
    e.chord = KeyChord{"G"};
    REQUIRE(scene.session.input(e, viewAt(at, direction)));
}

constexpr DVec3 kDown{0, 0, -1}, kUp{0, 0, 1};

} // namespace

TEST_CASE("D5 re-base: the re-based mesh is the affinely moved original mesh") {
    Document doc{parsed(fixture())};
    const auto corners = cornersOf(doc, kSide);
    const auto disp = dispOf(doc, kSide);
    const auto original = meshOf(corners, disp);
    // The face's plane is z = 64. In-plane scale and shear fix its normal; a rotation carries it.
    // On such maps the unit normal goes to the new unit normal, as the re-base assumes.
    const DMat3 inPlane = DMat3{DVec3{1.5, 0, 0}, DVec3{0.3, 0.75, 0}, DVec3{0, 0, 1}};
    const std::vector<std::pair<const char*, Transform>> cases{
        {"identity", Transform{}},
        {"translate", Transform::translate({16, -40, 8})},
        {"rotate about a tilted axis", Transform::rotate(glm::normalize(DVec3{1, 2, 3}), 37, {0, -1536, 32})},
        {"turn upside down", Transform::rotate({1, 0, 0}, 180, {0, -1536, 32})},
        {"in-plane scale and shear", Transform{inPlane, DVec3{-8, 24, 0}}},
        {"shear then rotate", Transform{Transform::rotate({0, 1, 0}, 25).linear() * inPlane, DVec3{100, 0, -50}}},
    };
    for (const auto& [name, transform] : cases) {
        CAPTURE(name);
        std::array<DVec3, 4> moved;
        for (std::size_t k = 0; k < 4; ++k) moved[k] = transform.point(corners[k]);
        const auto rebased = geometry::rebaseDisplacement(corners, moved, disp);
        REQUIRE(rebased);
        const auto mesh = meshOf(moved, *rebased);
        REQUIRE(mesh.positions.size() == original.positions.size());
        for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
            CAPTURE(i);
            CHECK(glm::distance(mesh.positions[i], transform.point(original.positions[i])) < 1e-6);
        }
        CHECK(mesh.alphas == original.alphas);
        CHECK(mesh.tags == original.tags);
        CHECK(mesh.triangles == original.triangles);
    }
    CHECK(*geometry::rebaseDisplacement(corners, corners, disp) == disp);
}

TEST_CASE("D5 re-base: a change that is not affine, or a degenerate quad, is an error") {
    Document doc{parsed(fixture())};
    const auto corners = cornersOf(doc, kSide);
    const auto disp = dispOf(doc, kSide);
    auto bent = corners;
    bent[2].z += 8; // One corner leaves the plane: the quad is no longer an affine image.
    CHECK_FALSE(geometry::rebaseDisplacement(corners, bent, disp));
    auto kite = corners;
    kite[2] += (kite[2] - kite[0]) * 0.25; // In plane, but not a parallelogram's image.
    CHECK_FALSE(geometry::rebaseDisplacement(corners, kite, disp));
    const std::array<DVec3, 4> flat{corners[0], corners[0], corners[0], corners[0]};
    CHECK_FALSE(geometry::rebaseDisplacement(corners, flat, disp));
    auto broken = disp;
    broken.distances.pop_back();
    CHECK_FALSE(geometry::rebaseDisplacement(corners, corners, broken));
}

TEST_CASE("D5 vertex tool: refuses to move the displaced side's own vertices") {
    VertexScene scene;
    const auto before = *scene.doc.write();
    const auto count = scene.doc.undoCount();
    click(scene, {96, -1440, 64}, kDown, false); // A corner of side 355 (above the bottom corner).
    REQUIRE(scene.tool().elements().size() == 1);
    grab(scene, {96, -1440, 64}, kDown);
    keys(scene, {"Z", "1", "6"});
    CHECK(scene.tool().readout().warn);
    CHECK(scene.tool().readout().status.find("displaced side's vertices") != std::string::npos);
    CHECK(scene.session.preview().empty());
    keys(scene, {"Enter"});
    CHECK(*scene.doc.write() == before);
    CHECK(scene.doc.undoCount() == count);
}

TEST_CASE("D5 vertex tool: moves the flat underside and re-bases the displacement in one step") {
    VertexScene scene;
    const auto corners = cornersOf(scene.doc, kSide);
    const auto mesh = meshOf(corners, dispOf(scene.doc, kSide));
    const auto dispinfo = dispinfoText(scene.doc, kSide);
    const auto before = *scene.doc.write();
    const auto count = scene.doc.undoCount();
    bool add = false;
    for (const DVec3 corner : {DVec3{96, -1440, 0}, DVec3{-96, -1440, 0}, DVec3{-96, -1632, 0}, DVec3{96, -1632, 0}}) {
        click(scene, corner, kUp, add); // From below, the bottom corner is the nearer one.
        add = true;
    }
    REQUIRE(scene.tool().elements().size() == 4);
    grab(scene, {0, -1536, 0}, kUp);
    keys(scene, {"Z", "-", "3", "2"});
    INFO("status: " << scene.tool().readout().status);
    CHECK_FALSE(scene.tool().readout().warn);
    REQUIRE_FALSE(scene.session.preview().empty());
    keys(scene, {"Enter"});
    REQUIRE(scene.doc.undoCount() == count + 1);
    CHECK(polyOf(scene.doc, scene.solid).bounds().min.z == doctest::Approx(-32));
    // The displaced face kept its side, its corners, its dispinfo and therefore its mesh.
    REQUIRE(solidOf(scene.doc, kSide) == scene.solid);
    sameCorners(cornersOf(scene.doc, kSide), corners);
    CHECK(dispinfoText(scene.doc, kSide) == dispinfo);
    const auto after = meshOf(cornersOf(scene.doc, kSide), dispOf(scene.doc, kSide));
    REQUIRE(after.positions.size() == mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) CHECK(glm::distance(after.positions[i], mesh.positions[i]) < 1e-9);
    REQUIRE(scene.session.perform("edit.undo"));
    CHECK(*scene.doc.write() == before);
}

TEST_CASE("D5 vertex tool: refuses a move of other vertices that reshapes the displaced face") {
    VertexScene scene;
    const auto before = *scene.doc.write();
    const auto count = scene.doc.undoCount();
    click(scene, {96, -1440, 0}, kUp, false);
    REQUIRE(scene.tool().elements().size() == 1);
    grab(scene, {96, -1440, 0}, kUp);
    keys(scene, {"Z", "1", "2", "8"}); // Above the top: the displaced quad cannot stay a face.
    const auto readout = scene.tool().readout();
    INFO("status: " << readout.status);
    CHECK(readout.warn);
    CHECK(readout.status.starts_with("Invalid move"));
    CHECK(readout.status.find("displace") != std::string::npos);
    CHECK(scene.session.preview().empty());
    keys(scene, {"Enter"});
    CHECK(*scene.doc.write() == before);
    CHECK(scene.doc.undoCount() == count);
}

// --- clip tool -----------------------------------------------------------------------------

namespace {
struct ClipScene {
    Document doc{parsed(fixture())};
    EditorSession session;
    Handle<Solid> solid;
    ClipScene() {
        solid = solidOf(doc, kSide);
        session.attach(doc);
        session.selection().setMode(SelectionMode::Solids);
        session.selection().select(doc.get(solid)->id);
        REQUIRE(session.perform("tool.clip"));
    }
    // Three clicks place the plane through a, b and c; the points lie outside the brush, so no face
    // supplies the third point. Each click lands on a pinned work plane through the point.
    void plane(DVec3 a, DVec3 b, DVec3 c) {
        for (const auto p : {a, b, c}) {
            const DVec3 direction = glm::normalize(DVec3{-1, -2, -3}); // No two points share a pixel.
            SurfacePlane surface;
            surface.normal = -direction;
            surface.origin = surface.normal * glm::dot(p, surface.normal);
            surface.uAxis = glm::normalize(glm::cross(DVec3{0, 0, 1}, direction));
            surface.vAxis = glm::cross(surface.normal, surface.uAxis);
            session.pinWorkPlane(surface);
            InputEvent e;
            e.kind = InputKind::Press;
            e.source = 1;
            e.button = PointerButton::Left;
            e.left = true;
            e.alt = session.preset().name == "Hammer";
            e.ctrl = session.preset().name == "Blender";
            ToolView view;
            view.cursorRay = {p - direction * 1000.0, direction};
            view.cameraPosition = view.cursorRay.origin;
            view.cameraForward = direction;
            view.cameraUp = glm::normalize(glm::cross(glm::cross(direction, DVec3{0, 0, 1}), direction));
            view.size = {800, 600};
            view.orthoHeight = 600;
            REQUIRE(session.input(e, view));
            e.kind = InputKind::Release;
            e.left = false;
            REQUIRE(session.input(e, view));
        }
    }
    void enter() {
        InputEvent e;
        e.kind = InputKind::KeyDown;
        e.source = 1;
        e.key = "Enter";
        REQUIRE(session.input(e, {}));
    }
    std::size_t solids() const {
        std::size_t n = 0;
        for (const auto h : nodes(doc)) n += std::holds_alternative<Handle<Solid>>(h);
        return n;
    }
};
// The plane z = 32 with its normal up (front is above) or down (front is below).
constexpr DVec3 kA{-200, -2000, 32}, kB{200, -2000, 32}, kC{-200, -1000, 32};
}

TEST_CASE("D5 clip: refuses a plane through the displaced face") {
    ClipScene scene;
    const auto before = *scene.doc.write();
    const auto count = scene.doc.undoCount();
    scene.plane({0, -2000, -200}, {0, -1000, -200}, {0, -2000, 200}); // x = 0 cuts side 355.
    CHECK(scene.session.preview().empty());
    CHECK(scene.session.activeToolObject().readout().warn);
    CHECK(scene.session.activeToolObject().readout().status.find("displaced face") != std::string::npos);
    scene.enter();
    CHECK(*scene.doc.write() == before);
    CHECK(scene.doc.undoCount() == count);
}

TEST_CASE("D5 clip: removes flat sides only, and the displaced side keeps its dispinfo") {
    ClipScene scene;
    const auto corners = cornersOf(scene.doc, kSide);
    const auto dispinfo = dispinfoText(scene.doc, kSide);
    const auto before = *scene.doc.write();
    const auto count = scene.doc.undoCount();
    const auto solids = scene.solids();
    SUBCASE("keep the piece with the displaced face") {
        scene.plane(kA, kB, kC); // Keep front: above z = 32.
        CHECK_FALSE(scene.session.activeToolObject().readout().warn);
        CHECK_FALSE(scene.session.preview().empty());
        scene.enter();
        REQUIRE(scene.doc.undoCount() == count + 1);
        CHECK(scene.solids() == solids);
        CHECK(polyOf(scene.doc, scene.solid).bounds().min.z == doctest::Approx(32));
    }
    SUBCASE("keep both, front above") {
        REQUIRE(scene.session.perform("clip.cycle_keep"));
        REQUIRE(scene.session.perform("clip.cycle_keep")); // Front, back, both.
        scene.plane(kA, kB, kC);
    }
    SUBCASE("keep both, front below: the source keeps the back piece") {
        REQUIRE(scene.session.perform("clip.cycle_keep"));
        REQUIRE(scene.session.perform("clip.cycle_keep"));
        scene.plane(kB, kA, kC);
    }
    if (scene.doc.undoCount() == count) { // The two keep-both subcases commit here.
        CHECK_FALSE(scene.session.activeToolObject().readout().warn);
        scene.enter();
        REQUIRE(scene.doc.undoCount() == count + 1);
        CHECK(scene.solids() == solids + 1);
        CHECK(polyOf(scene.doc, scene.solid).bounds().min.z == doctest::Approx(32));
        std::size_t displaced = 0;
        for (const auto h : nodes(scene.doc)) if (const auto* solid = std::get_if<Handle<Solid>>(&h))
            displaced += hasDisplacement(*scene.doc.get(*solid)->block);
        CHECK(displaced == 1);
    }
    REQUIRE(solidOf(scene.doc, kSide) == scene.solid);
    sameCorners(cornersOf(scene.doc, kSide), corners);
    CHECK(dispinfoText(scene.doc, kSide) == dispinfo);
    REQUIRE(scene.session.perform("edit.undo"));
    CHECK(*scene.doc.write() == before);
}

TEST_CASE("D5 clip: refuses to clip the displaced face away") {
    ClipScene scene;
    const auto before = *scene.doc.write();
    const auto count = scene.doc.undoCount();
    REQUIRE(scene.session.perform("clip.cycle_keep")); // Keep back: below z = 32.
    scene.plane(kA, kB, kC);
    CHECK(scene.session.preview().empty());
    CHECK(scene.session.activeToolObject().readout().warn);
    CHECK(scene.session.activeToolObject().readout().status.find("clip away a displaced face") != std::string::npos);
    scene.enter();
    CHECK(*scene.doc.write() == before);
    CHECK(scene.doc.undoCount() == count);
}

// --- carve, hollow, merge ------------------------------------------------------------------

TEST_CASE("D5 carve, hollow and merge refuse a displacement solid with a per-tool message") {
    Document doc{parsed(fixture())};
    const auto displaced = solidOf(doc, kSide);
    const auto box = addBox(doc, {-32, -1600, -16}, {32, -1500, 32}); // Overlaps solid 637.
    EditorSession session;
    session.attach(doc);
    session.selection().setMode(SelectionMode::Solids);
    const auto before = *doc.write();
    const auto count = doc.undoCount();
    const auto refused = [](const auto& result, const char* text) {
        REQUIRE_FALSE(result);
        INFO(result.error().message);
        CHECK(result.error().message.find(text) != std::string::npos);
        CHECK(result.error().message.find("Destroy displacement") != std::string::npos);
    };
    session.selection().select(doc.get(displaced)->id);
    refused(carve(session), "cannot carve");
    refused(hollow(session, 8), "cannot be hollowed");
    refused(hollow(session, -8), "cannot be hollowed");
    session.selection().add(id(doc, box));
    refused(planMerge(session), "cannot be merged");
    session.selection().select(id(doc, box));
    refused(carve(session), "displacement target");
    CHECK(*doc.write() == before);
    CHECK(doc.undoCount() == count);
}
