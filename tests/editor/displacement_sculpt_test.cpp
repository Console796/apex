// Plan 0.8 D6 (0.8.4): the Displacement tool's Sculpt and Paint alpha strokes, driven through
// EditorSession::input as a viewport would: replay, one undo step per stroke, sewn neighbours,
// orthographic rays and out-of-range alphas.
#include "fixtures.hpp"
#include "document/displacement.hpp"
#include "editor/displacement_edit.hpp"
#include "editor/displacement_tool.hpp"
#include "editor/editor_session.hpp"
#include "editor/map_problems.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace editor_test;

namespace {

const char* kEmpty = "versioninfo\n{\n\"editorversion\" \"400\"\n}\nworld\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n}\n";

std::string fixture() {
    std::ifstream stream(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf" / "disp" / "test1_disp.vmf", std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

SideId sideFacing(const Document& doc, NodeHandle solid, DVec3 normal) {
    const auto geometry = geometry::solidGeometry(*doc.get(solid)->block);
    REQUIRE(geometry);
    for (const auto& face : geometry->poly.faces)
        if (glm::dot(face.plane.normal, normal) > 0.999) return static_cast<SideId>(face.sideId);
    FAIL("no such face");
    return 0;
}

NodeHandle world(const Document& doc) { return *doc.handle(doc.roots().front()); }

int problems(const Document& doc, ProblemKind kind) {
    const auto found = ProblemRegistry::builtin().run(ProblemContext{doc});
    return static_cast<int>(std::ranges::count(found, kind, &Problem::kind));
}

geometry::DisplacementMesh mesh(const Document& doc, SideId side) {
    const auto base = sideDisplacement(doc, side).value();
    REQUIRE(base.disp);
    return geometry::displacementMesh(base.corners, *base.disp).value();
}

// One recorded viewport gesture: the events and the ray each was made with.
struct Recorded {
    InputEvent event;
    ToolView view;
};
using Recording = std::vector<Recorded>;

ToolView rayView(Ray ray, bool orthographic = false) {
    ToolView view;
    view.cursorRay = ray;
    view.size = {800, 600};
    view.orthographic = orthographic;
    return view;
}

// A left-button drag through `rays`, as a viewport reports it: a pane that sees its press consumed
// sends CancelPointer straight after it (PaneWindow::handleMouse), which must not end the stroke.
Recording drag(const std::vector<Ray>& rays, bool ctrl = false, bool orthographic = false) {
    Recording result;
    for (std::size_t i = 0; i < rays.size(); ++i) {
        InputEvent event;
        event.kind = i == 0 ? InputKind::Press : InputKind::Move;
        event.button = i == 0 ? PointerButton::Left : PointerButton::None;
        event.left = true;
        event.ctrl = ctrl;
        result.push_back({event, rayView(rays[i], orthographic)});
        if (i == 0) {
            InputEvent cancelPointer;
            cancelPointer.kind = InputKind::CancelPointer;
            result.push_back({cancelPointer, rayView(rays[i], orthographic)});
        }
    }
    InputEvent release;
    release.kind = InputKind::Release;
    release.button = PointerButton::Left;
    release.ctrl = ctrl;
    result.push_back({release, rayView(rays.back(), orthographic)});
    return result;
}

// Straight down onto the points.
std::vector<Ray> fromAbove(std::initializer_list<DVec2> points) {
    std::vector<Ray> rays;
    for (const auto& p : points) rays.push_back({{p.x, p.y, 1024.0}, {0, 0, -1}});
    return rays;
}

struct Session {
    Document doc;
    EditorSession session;
    explicit Session(std::string_view source) : doc(parsed(source)) {
        session.attach(doc);
        REQUIRE(session.perform("tool.displacement"));
    }
    DisplacementTool& tool() { return dynamic_cast<DisplacementTool&>(session.activeToolObject()); }
    void select(std::initializer_list<SideId> sides) {
        session.selection().setMode(SelectionMode::Faces);
        session.selection().clear();
        for (const auto side : sides) session.selection().add(side);
    }
    void play(const Recording& recording) {
        for (const auto& [event, view] : recording) session.input(event, view);
    }
    // Displaces the top faces of boxes with the given power each, as the tool's Create does.
    std::vector<SideId> terrain(const std::vector<std::pair<DVec3, DVec3>>& boxes, std::vector<int> powers) {
        std::vector<SideId> tops;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const auto box = addBox(doc, boxes[i].first, boxes[i].second, world(doc));
            tops.push_back(sideFacing(doc, box, {0, 0, 1}));
            const std::vector<SideId> one{tops.back()};
            REQUIRE(createDisplacements(session, one, powers[i], 0.0));
        }
        session.selection().clear();
        return tops;
    }
};

// A two-patch terrain scene used by several cases: [0,128] and [128,256] in x, 128 deep.
const std::vector<std::pair<DVec3, DVec3>> kPair{{{0, 0, 0}, {128, 128, 64}}, {{128, 0, 0}, {256, 128, 64}}};

} // namespace

TEST_CASE("a recorded sculpt and paint session replays to the same dispinfo") {
    const auto record = [](Session& s) {
        s.tool().setMode(DisplacementTool::Mode::Sculpt);
        s.tool().setParameter("radius", 48);
        s.tool().setParameter("amount", 6);
        s.play(drag(fromAbove({{40, 40}, {70, 52}, {120, 64}, {170, 70}, {200, 90}})));
        s.tool().setParameter("brush", double(geometry::SculptBrush::Noise));
        s.tool().setParameter("seed", 7);
        s.play(drag(fromAbove({{60, 80}, {100, 90}, {150, 60}})));
        s.tool().setParameter("brush", double(geometry::SculptBrush::Smooth));
        s.tool().setParameter("direction", double(geometry::SculptDirection::VertexNormal));
        s.play(drag(fromAbove({{90, 64}, {140, 64}})));
        s.tool().setMode(DisplacementTool::Mode::PaintAlpha);
        s.tool().setParameter("alpha", 200);
        s.play(drag(fromAbove({{20, 20}, {230, 110}})));
    };
    Session a(kEmpty), b(kEmpty);
    a.terrain(kPair, {3, 3});
    b.terrain(kPair, {3, 3});
    const auto before = a.doc.write().value();
    REQUIRE(b.doc.write().value() == before);
    const auto undo = a.doc.undoCount();
    record(a);
    record(b);
    CHECK(a.doc.undoCount() == undo + 4);
    const auto after = a.doc.write().value();
    CHECK(after != before);
    CHECK(b.doc.write().value() == after);
    // The noise brush and the stroke spacing are deterministic, not just repeatable in-process:
    // a third session whose working set is read fresh before each stroke agrees too.
    Session c(kEmpty);
    c.terrain(kPair, {3, 3});
    c.tool().setMode(DisplacementTool::Mode::Sculpt);
    c.tool().setParameter("radius", 48);
    c.tool().setParameter("amount", 6);
    c.play(drag(fromAbove({{40, 40}, {70, 52}, {120, 64}, {170, 70}, {200, 90}})));
    // Re-reading after an unrelated edit and its undo drops the cached working set.
    addBox(c.doc, {512, 512, 0}, {576, 576, 64});
    REQUIRE(c.doc.undo());
    c.tool().setParameter("brush", double(geometry::SculptBrush::Noise));
    c.tool().setParameter("seed", 7);
    c.play(drag(fromAbove({{60, 80}, {100, 90}, {150, 60}})));
    c.tool().setParameter("brush", double(geometry::SculptBrush::Smooth));
    c.tool().setParameter("direction", double(geometry::SculptDirection::VertexNormal));
    c.play(drag(fromAbove({{90, 64}, {140, 64}})));
    c.tool().setMode(DisplacementTool::Mode::PaintAlpha);
    c.tool().setParameter("alpha", 200);
    c.play(drag(fromAbove({{20, 20}, {230, 110}})));
    CHECK(c.doc.write().value() == after);
}

TEST_CASE("undo takes back exactly one stroke, and the preview is gone after release") {
    Session s(kEmpty);
    const auto tops = s.terrain(kPair, {2, 2});
    s.tool().setMode(DisplacementTool::Mode::Sculpt);
    s.tool().setParameter("radius", 40);
    const auto original = s.doc.write().value();
    const auto undo = s.doc.undoCount();

    // Radius 40 from x <= 80: the left patch only.
    auto first = drag(fromAbove({{32, 32}, {64, 64}, {80, 80}}));
    // Mid-stroke: previewed, not written. Viewports sync the preview on a session notification, so
    // the moves after the press must notify for the mesh to follow the stroke.
    int notified = 0;
    const auto observer = s.session.observe([&] { ++notified; });
    s.session.input(first[0].event, first[0].view);
    s.session.input(first[1].event, first[1].view);
    const int atPress = notified;
    for (std::size_t i = 2; i + 1 < first.size(); ++i) s.session.input(first[i].event, first[i].view);
    CHECK(notified > atPress);
    s.session.unobserve(observer);
    CHECK(s.tool().stroking());
    CHECK_FALSE(s.session.preview().displacements().empty());
    CHECK(s.session.preview().replacements().size() == 1);
    CHECK(s.doc.undoCount() == undo);
    CHECK(s.doc.write().value() == original);
    s.session.input(first.back().event, first.back().view);
    CHECK_FALSE(s.tool().stroking());
    CHECK(s.session.preview().empty());
    CHECK(s.doc.undoCount() == undo + 1);
    CHECK(s.doc.undoLabels().back() == "Sculpt displacement");
    const auto afterFirst = s.doc.write().value();
    CHECK(afterFirst != original);

    s.play(drag(fromAbove({{160, 64}, {224, 64}}), /*ctrl=*/true));
    CHECK(s.doc.undoCount() == undo + 2);
    // Ctrl lowers: the right patch dips below its base.
    const auto lowered = mesh(s.doc, tops[1]);
    CHECK(std::ranges::any_of(lowered.positions, [](DVec3 p) { return p.z < 63.0; }));

    s.tool().setMode(DisplacementTool::Mode::PaintAlpha);
    s.play(drag(fromAbove({{64, 64}})));
    CHECK(s.doc.undoLabels().back() == "Paint displacement alpha");
    CHECK(s.doc.undoCount() == undo + 3);

    REQUIRE(s.doc.undo());
    REQUIRE(s.doc.undo());
    CHECK(s.doc.write().value() == afterFirst);
    REQUIRE(s.doc.undo());
    CHECK(s.doc.write().value() == original);
    // After undo the next stroke starts from the restored document, not the tool's old copy.
    s.tool().setMode(DisplacementTool::Mode::Sculpt);
    s.play(drag(fromAbove({{200, 100}})));
    CHECK(s.doc.undoCount() == undo + 1);
    for (const auto& p : mesh(s.doc, tops[0]).positions) CHECK(p.z == 64.0);
}

TEST_CASE("Esc cancels a stroke; a press off every displacement does nothing") {
    Session s(kEmpty);
    s.terrain(kPair, {2, 2});
    s.tool().setMode(DisplacementTool::Mode::Sculpt);
    const auto original = s.doc.write().value();
    const auto undo = s.doc.undoCount();
    auto gesture = drag(fromAbove({{32, 32}, {64, 64}}));
    s.session.input(gesture[0].event, gesture[0].view);
    s.session.input(gesture[1].event, gesture[1].view);
    REQUIRE(s.tool().stroking());
    InputEvent escape;
    escape.kind = InputKind::KeyDown;
    escape.key = "Escape";
    CHECK(s.session.input(escape, gesture[1].view));
    CHECK_FALSE(s.tool().stroking());
    CHECK(s.session.preview().empty());
    s.session.input(gesture.back().event, gesture.back().view);
    CHECK(s.doc.undoCount() == undo);
    CHECK(s.doc.write().value() == original);

    // A viewport Cancel (focus lost, pane hidden) ends it too; CancelPointer, sent with every press, does not.
    s.session.input(gesture[0].event, gesture[0].view);
    s.session.input(gesture[1].event, gesture[1].view);
    REQUIRE(s.tool().stroking());
    InputEvent cancel;
    cancel.kind = InputKind::Cancel;
    CHECK(s.session.input(cancel, gesture[1].view));
    CHECK_FALSE(s.tool().stroking());
    s.session.input(gesture.back().event, gesture.back().view);
    CHECK(s.doc.undoCount() == undo);

    s.play(drag(fromAbove({{1000, 1000}, {1100, 1000}})));
    CHECK(s.doc.undoCount() == undo);
    CHECK(s.tool().notice() == "Point at a displacement to sculpt");
    // Enter does not apply in a brush mode.
    CHECK_FALSE(s.tool().apply());
}

TEST_CASE("a stroke across a sewn edge moves both sides, so no edge opens") {
    // Equal powers, then a T-edge: power 3 against power 2.
    for (const auto& powers : {std::vector<int>{3, 3}, std::vector<int>{3, 2}}) {
        CAPTURE(powers[1]);
        Session s(kEmpty);
        const auto tops = s.terrain(kPair, powers);
        REQUIRE(problems(s.doc, ProblemKind::DisplacementUnsewn) == 0);
        s.tool().setMode(DisplacementTool::Mode::Sculpt);
        s.tool().setParameter("radius", 40);
        s.tool().setParameter("amount", 9);
        s.tool().setParameter("falloff", double(geometry::BrushFalloff::Linear));
        // The shared vertex at (128, 64) on each patch.
        const auto heights = [&] {
            std::vector<double> found;
            for (const auto side : tops)
                for (const auto& p : mesh(s.doc, side).positions)
                    if (std::abs(p.x - 128.0) < 1e-6 && std::abs(p.y - 64.0) < 1e-6) found.push_back(p.z);
            REQUIRE(found.size() == 2);
            return found;
        };
        // Along the shared edge: it rose on both patches, to the same height.
        s.play(drag(fromAbove({{128, 8}, {128, 40}, {124, 80}, {132, 120}})));
        CHECK(problems(s.doc, ProblemKind::DisplacementUnsewn) == 0);
        auto raised = heights();
        CHECK(raised[0] > 64.0);
        CHECK(raised[0] == doctest::Approx(raised[1]).epsilon(1e-9));
        // Across it with Noise.
        s.tool().setParameter("brush", double(geometry::SculptBrush::Noise));
        s.play(drag(fromAbove({{90, 60}, {170, 70}})));
        CHECK(problems(s.doc, ProblemKind::DisplacementUnsewn) == 0);
        raised = heights();
        CHECK(raised[0] == doctest::Approx(raised[1]).epsilon(1e-9));
    }
}

TEST_CASE("with one side of a seam selected, only the seam's vertices move on the other") {
    Session s(kEmpty);
    const auto tops = s.terrain(kPair, {3, 3});
    s.select({tops[0]});
    s.tool().setMode(DisplacementTool::Mode::Sculpt);
    s.tool().setParameter("radius", 40);
    s.tool().setParameter("falloff", double(geometry::BrushFalloff::Constant));
    s.play(drag(fromAbove({{128, 64}})));
    CHECK(problems(s.doc, ProblemKind::DisplacementUnsewn) == 0);
    std::size_t raisedLeft = 0;
    for (const auto& p : mesh(s.doc, tops[0]).positions) raisedLeft += p.z > 64.0;
    CHECK(raisedLeft > 3);
    for (const auto& p : mesh(s.doc, tops[1]).positions) {
        CAPTURE(p);
        if (std::abs(p.x - 128.0) < 1e-6 && std::abs(p.y - 64.0) < 40.0) CHECK(p.z > 64.0); // On the seam.
        else CHECK(p.z == 64.0);                                                            // Not selected.
    }
}

TEST_CASE("sculpting from an orthographic side view moves a wall displacement along its normal") {
    Session s(kEmpty);
    const auto box = addBox(s.doc, {0, 0, 0}, {64, 128, 128}, world(s.doc));
    const auto wall = sideFacing(s.doc, box, {1, 0, 0});
    REQUIRE(createDisplacements(s.session, std::vector<SideId>{wall}, 2, 0.0));
    s.tool().setMode(DisplacementTool::Mode::Sculpt);
    s.tool().setParameter("radius", 48);
    s.tool().setParameter("amount", 5);
    // An orthographic side view looks along -x from far away.
    s.play(drag({Ray{{100000, 64, 64}, {-1, 0, 0}}, Ray{{100000, 70, 64}, {-1, 0, 0}}}, false, true));
    CHECK(s.doc.undoLabels().back() == "Sculpt displacement");
    const auto pushed = mesh(s.doc, wall);
    double most = 0.0;
    for (const auto& p : pushed.positions) {
        most = std::max(most, p.x - 64.0);
        CHECK(p.x >= 64.0);
    }
    CHECK(most == doctest::Approx(5.0));
    // Stored as Hammer stores a painted vertex: a unit normal and a non-negative distance.
    const auto disp = *document::displacement(s.doc, wall).value();
    for (std::size_t i = 0; i < disp.vertexCount(); ++i) {
        CHECK(disp.distances[i] >= 0.0);
        if (disp.distances[i] > 0.0) CHECK(glm::length(disp.normals[i]) == doctest::Approx(1.0));
    }
}

TEST_CASE("a sculpt stroke retags by slope as Create and Sew do") {
    Session s(kEmpty);
    const auto tops = s.terrain({{{0, 0, 0}, {128, 128, 64}}}, {3});
    s.tool().setMode(DisplacementTool::Mode::Sculpt);
    s.tool().setParameter("radius", 24);
    s.tool().setParameter("amount", 40);
    s.tool().setParameter("falloff", double(geometry::BrushFalloff::Linear));
    s.play(drag(fromAbove({{64, 64}})));
    const auto base = sideDisplacement(s.doc, tops[0]).value();
    auto retagged = *base.disp;
    REQUIRE(geometry::retagDisplacement(base.corners, retagged));
    CHECK(retagged.triangleTags == base.disp->triangleTags);
    // The spike is steep: some triangles lost the walkable bit.
    CHECK(std::ranges::any_of(base.disp->triangleTags,
                              [](int tag) { return (tag & geometry::Displacement::kTagWalkable) == 0; }));
}

TEST_CASE("paint alpha leaves out-of-range alphas alone unless painted over") {
    Session s(fixture());
    constexpr SideId kSide = 355; // test1_disp.vmf's displaced top face; column 0's alphas are -96.
    const auto alphaRange = problems(s.doc, ProblemKind::DisplacementAlphaRange);
    REQUIRE(alphaRange == 1);
    const auto before = *document::displacement(s.doc, kSide).value();
    const auto grid = mesh(s.doc, kSide);
    // Row 4, column 0: one vertex of the -96 column.
    const std::size_t target = 4 * 9 + 0;
    REQUIRE(before.alphas[target] == -96.0);
    s.tool().setMode(DisplacementTool::Mode::PaintAlpha);
    s.tool().setParameter("radius", 10);
    s.tool().setParameter("strength", 1);
    s.tool().setParameter("falloff", double(geometry::BrushFalloff::Constant));
    s.tool().setParameter("alpha", 200);
    const auto at = grid.positions[target];
    s.play(drag({Ray{at + DVec3{0, 0, 256}, {0, 0, -1}}}));
    const auto after = *document::displacement(s.doc, kSide).value();
    for (std::size_t i = 0; i < after.vertexCount(); ++i) {
        CAPTURE(i);
        CHECK(after.alphas[i] == (i == target ? 200.0 : before.alphas[i]));
    }
    // Geometry is untouched by paint; the rest of column 0 is still out of range.
    CHECK(after.distances == before.distances);
    CHECK(after.triangleTags == before.triangleTags);
    CHECK(problems(s.doc, ProblemKind::DisplacementAlphaRange) == 1);
    CHECK(s.doc.undoLabels().back() == "Paint displacement alpha");
}

TEST_CASE("the Sculpt and Paint alpha readouts show their brush parameters") {
    Session s(kEmpty);
    s.tool().setMode(DisplacementTool::Mode::Sculpt);
    auto readout = s.tool().readout();
    std::vector<std::string> ids;
    for (const auto& p : readout.parameters) ids.push_back(p.id);
    CHECK(ids == std::vector<std::string>{"mode", "brush", "direction", "radius", "falloff", "amount"});
    CHECK(readout.parameters[0].choices.size() == 4);
    CHECK(readout.detail == "Sculpt");
    s.tool().setParameter("brush", double(geometry::SculptBrush::SetHeight));
    ids.clear();
    for (const auto& p : s.tool().readout().parameters) ids.push_back(p.id);
    CHECK(ids == std::vector<std::string>{"mode", "brush", "direction", "radius", "falloff", "strength", "height"});
    s.tool().setParameter("mode", 3);
    readout = s.tool().readout();
    ids.clear();
    for (const auto& p : readout.parameters) ids.push_back(p.id);
    CHECK(ids == std::vector<std::string>{"mode", "radius", "falloff", "strength", "alpha"});
    CHECK(readout.detail == "Paint alpha");
    CHECK(s.tool().setParameter("alpha", 300));
    CHECK(s.tool().paintSettings().alpha == 255.0);
    CHECK(s.tool().setParameter("radius", -5));
    CHECK(s.tool().paintSettings().radius == 1.0);
    CHECK_FALSE(s.tool().setParameter("nonsense", 1));
}
