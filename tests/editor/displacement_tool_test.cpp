// Plan 0.8 D5/D6 (0.8.3): the Displacement tool's Create, Subdivide and Sew modes, Destroy
// displacement, the unsewn-edge problem and the Face Properties edits behind power and flags.
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

constexpr SideId kSide = 355; // test1_disp.vmf's displaced top face.
const char* kEmpty = "versioninfo\n{\n\"editorversion\" \"400\"\n}\nworld\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n}\n";

std::string fixture() {
    std::ifstream stream(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf" / "disp" / "test1_disp.vmf", std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

const kv1::Node& sideBlock(const Document& doc, SideId id) {
    const auto location = doc.side(id);
    REQUIRE(location);
    std::size_t face = 0;
    for (const auto& child : doc.get(location->solid)->block->children()) {
        if (child.kind() != kv1::NodeKind::Block || child.key() != "side") continue;
        if (face++ == location->faceIndex) return child;
    }
    FAIL("side not found");
    throw;
}

// The side of a solid whose outward normal is `normal`.
SideId sideFacing(const Document& doc, NodeHandle solid, DVec3 normal) {
    const auto geometry = geometry::solidGeometry(*doc.get(solid)->block);
    REQUIRE(geometry);
    for (const auto& face : geometry->poly.faces)
        if (glm::dot(face.plane.normal, normal) > 0.999) return static_cast<SideId>(face.sideId);
    FAIL("no such face");
    return 0;
}

NodeHandle world(const Document& doc) { return *doc.handle(doc.roots().front()); }

int unsewn(const Document& doc) {
    const auto problems = ProblemRegistry::builtin().run(ProblemContext{doc});
    return static_cast<int>(std::ranges::count(problems, ProblemKind::DisplacementUnsewn, &Problem::kind));
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
};

} // namespace

TEST_CASE("Create writes every dispinfo row Hammer writes, and the block reads back as created") {
    const auto source = fixture();
    Session s(source);
    const auto original = *sideBlock(s.doc, kSide).find("dispinfo");
    s.select({kSide});
    REQUIRE(s.session.perform("edit.destroy_displacement"));
    CHECK_FALSE(document::isDisplaced(sideBlock(s.doc, kSide)));
    REQUIRE(s.session.perform("tool.displacement"));
    s.tool().setParameter("mode", 0);
    s.tool().setParameter("power", 3);
    REQUIRE(s.session.perform("displacement.apply"));
    CHECK(s.doc.undoLabels().back() == "Create displacement");
    CHECK(s.tool().notice().starts_with("Created 1 displacement at power 3"));

    const auto& created = *sideBlock(s.doc, kSide).find("dispinfo");
    // The same keys and rows in the same order as Strata Hammer's block.
    REQUIRE(created.children().size() == original.children().size());
    for (std::size_t i = 0; i < original.children().size(); ++i) {
        const auto& want = original.children()[i];
        const auto& got = created.children()[i];
        CAPTURE(want.key());
        CHECK(got.key() == want.key());
        CHECK(got.kind() == want.kind());
        REQUIRE(got.children().size() == want.children().size());
        for (std::size_t r = 0; r < want.children().size(); ++r) {
            CHECK(got.children()[r].key() == want.children()[r].key());
            // A row has as many numbers as Hammer's.
            const auto count = [](const kv1::Node& row) {
                const auto text = row.decodedValue().value_or("");
                return std::ranges::count(text, ' ');
            };
            CHECK(count(got.children()[r]) == count(want.children()[r]));
        }
    }
    // Rows a sculpt and paint do not touch are Hammer's, byte for byte.
    for (const auto* key : {"power", "startposition", "flags", "elevation", "subdiv"})
        CHECK(created.find(key)->decodedValue().value() == original.find(key)->decodedValue().value());
    for (const auto* block : {"offsets", "offset_normals", "allowed_verts"}) {
        CAPTURE(block);
        const auto& a = *created.find(block);
        const auto& b = *original.find(block);
        for (std::size_t r = 0; r < b.children().size(); ++r)
            CHECK(a.children()[r].decodedValue().value() == b.children()[r].decodedValue().value());
    }
    CHECK(created.find("normals")->children()[0].decodedValue().value() ==
          "0 0 1 0 0 1 0 0 1 0 0 1 0 0 1 0 0 1 0 0 1 0 0 1 0 0 1");
    CHECK(created.find("triangle_tags")->children()[0].decodedValue().value() == "9 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9");

    // parse(write(create)) is the created value, and survives a save and reload.
    const auto base = sideDisplacement(s.doc, kSide).value();
    const auto expected = geometry::createDisplacement(base.corners, 3, 0).value();
    CHECK(base.disp == expected);
    Document reloaded(parsed(s.doc.write().value()));
    CHECK(document::displacement(reloaded, kSide).value() == expected);
    // The created mesh is the flat base grid.
    const auto mesh = geometry::displacementMesh(base.corners, expected).value();
    for (const auto& p : mesh.positions) CHECK(p.z == 64.0);
}

TEST_CASE("Hammer's triangle tags on test1 are the slope rule's") {
    Document doc(parsed(fixture()));
    const auto base = sideDisplacement(doc, kSide).value();
    REQUIRE(base.disp);
    auto retagged = *base.disp;
    std::ranges::fill(retagged.triangleTags, 0);
    REQUIRE(geometry::retagDisplacement(base.corners, retagged));
    CHECK(retagged.triangleTags == base.disp->triangleTags);
}

TEST_CASE("Destroy displacement is one labelled step, and undo restores the block byte for byte") {
    const auto source = fixture();
    Session s(source);
    s.select({kSide, 356}); // 356 is flat: ignored.
    REQUIRE(s.session.perform("edit.destroy_displacement"));
    CHECK(s.doc.undoCount() == 1);
    CHECK(s.doc.undoLabels().back() == "Destroy displacement");
    CHECK(s.session.topologyMessage() == "Destroyed 1 displacement; the faces are flat again");
    CHECK_FALSE(document::isDisplaced(sideBlock(s.doc, kSide)));
    CHECK_FALSE(document::displacement(s.doc, kSide).value());
    REQUIRE(s.doc.undo());
    CHECK(s.doc.write().value() == source);
    // Nothing displaced selected: refused, no undo step.
    s.select({356});
    CHECK_FALSE(s.session.perform("edit.destroy_displacement"));
    CHECK(s.session.topologyMessage() == "No displaced face is selected");
    CHECK(s.doc.undoCount() == 0);
    // In solid mode it takes every displaced side of the selected solids.
    s.session.selection().setMode(SelectionMode::Solids);
    s.session.selection().select(s.doc.get(s.doc.side(kSide)->solid)->id);
    REQUIRE(s.session.perform("edit.destroy_displacement"));
    CHECK_FALSE(document::isDisplaced(sideBlock(s.doc, kSide)));
}

TEST_CASE("Create refuses faces that are not quads and skips displaced ones") {
    Session s(kEmpty);
    NodeHandle wedge;
    {
        auto tx = s.doc.begin("wedge");
        wedge = tx.create(solidBlock(geometry_test::build(geometry_test::wedgeSides())), world(s.doc));
        tx.commit();
    }
    const auto triangle = sideFacing(s.doc, wedge, {0, 0, 1});
    const auto slope = sideFacing(s.doc, wedge, glm::normalize(DVec3{1, 1, 0}));
    const auto undo = s.doc.undoCount();
    s.select({triangle});
    CHECK_FALSE(s.tool().apply());
    CHECK(s.tool().notice().find("not a quad") != std::string::npos);
    CHECK(s.doc.undoCount() == undo);

    s.select({triangle, slope});
    CHECK(s.tool().apply());
    CHECK(s.doc.undoCount() == undo + 1);
    CHECK(document::displacement(s.doc, slope).value());
    CHECK_FALSE(document::displacement(s.doc, triangle).value());
    CHECK(s.tool().notice().find("refused 1 face that is not a quad") != std::string::npos);
    // Again: the slope is already displaced, the triangle still refused, nothing created.
    CHECK_FALSE(s.tool().apply());
    CHECK(s.tool().notice().find("already displaced") != std::string::npos);
    CHECK(s.doc.undoCount() == undo + 1);
}

TEST_CASE("the Displacement tool's readout, parameters, clicks and Enter") {
    Session s(kEmpty);
    const auto box = addBox(s.doc, {0, 0, 0}, {128, 128, 64}, world(s.doc));
    const auto top = sideFacing(s.doc, box, {0, 0, 1});
    auto readout = s.tool().readout();
    REQUIRE(readout.parameters.size() == 3);
    CHECK(readout.parameters[0].choices == std::vector<std::string>{"Create", "Subdivide", "Sew"});
    CHECK(readout.parameters[1].id == "power");
    CHECK(readout.parameters[2].id == "elevation");
    CHECK(s.tool().setParameter("power", 9));
    CHECK(s.tool().power() == 4);
    CHECK(s.tool().setParameter("elevation", 16));
    // A click on the top face selects it in face mode; Enter creates.
    InputEvent press;
    press.kind = InputKind::Press;
    press.button = PointerButton::Left;
    press.left = true;
    ToolView view;
    view.cursorRay = {{64, 64, 512}, {0, 0, -1}};
    view.size = {800, 600};
    REQUIRE(s.session.input(press, view));
    CHECK(s.session.selection().mode() == SelectionMode::Faces);
    REQUIRE(s.session.selection().ordered().size() == 1);
    CHECK(std::get<SideId>(s.session.selection().ordered().front()) == top);
    InputEvent enter;
    enter.kind = InputKind::KeyDown;
    enter.key = "Enter";
    REQUIRE(s.session.input(enter, view));
    const auto disp = document::displacement(s.doc, top).value();
    REQUIRE(disp);
    CHECK(disp->power == 4);
    CHECK(disp->elevation == 16);
    // Subdivide and Sew show only the mode.
    s.tool().setMode(DisplacementTool::Mode::Subdivide);
    readout = s.tool().readout();
    CHECK(readout.parameters.size() == 1);
    CHECK(readout.detail == "Subdivide");
    REQUIRE(s.session.perform("displacement.apply"));
    CHECK(document::displacement(s.doc, top).value()->subdiv == 1);
    CHECK(s.doc.undoLabels().back() == "Subdivide displacements");
}

TEST_CASE("the unsewn check finds an open edge and not a closed one, and Sew closes it") {
    Session s(kEmpty);
    const auto left = addBox(s.doc, {0, 0, 0}, {128, 128, 64}, world(s.doc));
    const auto right = addBox(s.doc, {128, 0, 0}, {256, 128, 64}, world(s.doc));
    const auto a = sideFacing(s.doc, left, {0, 0, 1}), b = sideFacing(s.doc, right, {0, 0, 1});
    s.select({a, b});
    s.tool().setParameter("power", 2);
    REQUIRE(s.tool().apply());
    CHECK(unsewn(s.doc) == 0);

    // Raise one vertex on the shared edge (x = 128) of the right patch.
    const auto raise = [&](double amount) {
        auto base = sideDisplacement(s.doc, b).value();
        const auto mesh = geometry::displacementMesh(base.corners, *base.disp).value();
        for (std::size_t i = 0; i < mesh.positions.size(); ++i)
            if (std::abs(mesh.positions[i].x - 128.0) < 1e-9 && std::abs(mesh.positions[i].y - 64.0) < 1e-9)
                base.disp->distances[i] = amount;
        s.session.edit("raise", [&](document::Transaction& tx) { tx.setDisplacement(b, *base.disp); });
    };
    raise(0.05);
    CHECK(unsewn(s.doc) == 0); // Within 0.1 units.
    raise(1.0);
    REQUIRE(unsewn(s.doc) == 1);
    const auto problems = ProblemRegistry::builtin().run(ProblemContext{s.doc});
    const auto found = std::ranges::find(problems, ProblemKind::DisplacementUnsewn, &Problem::kind);
    CHECK(found->text.find("gap 1 units") != std::string::npos);
    CHECK(found->objects.size() == 2);

    s.tool().setMode(DisplacementTool::Mode::Sew);
    REQUIRE(s.tool().apply());
    CHECK(s.doc.undoLabels().back() == "Sew displacements");
    CHECK(unsewn(s.doc) == 0);
    // Sewn neighbours share the midpoint of the gap.
    const auto mesh = [&](SideId side) {
        const auto base = sideDisplacement(s.doc, side).value();
        return geometry::displacementMesh(base.corners, *base.disp).value();
    };
    for (const auto& p : mesh(a).positions)
        if (std::abs(p.x - 128.0) < 1e-9 && std::abs(p.y - 64.0) < 1e-9) CHECK(p.z == doctest::Approx(64.5));
    // Sew with one displacement selected is refused.
    s.select({a});
    CHECK_FALSE(s.tool().apply());
    CHECK(s.tool().notice().starts_with("Select two or more displacements"));
}

TEST_CASE("power and flag edits: flags toggle per bit, power rebuilds only a flat displacement") {
    Session s(kEmpty);
    const auto box = addBox(s.doc, {0, 0, 0}, {128, 128, 64}, world(s.doc));
    const auto top = sideFacing(s.doc, box, {0, 0, 1});
    const std::vector<SideId> sides{top};
    s.select({top});
    REQUIRE(s.tool().apply());

    using geometry::Displacement;
    REQUIRE(setDisplacementFlags(s.session, sides, Displacement::kFlagNoPhys | Displacement::kFlagNoRay, true));
    CHECK(document::displacement(s.doc, top).value()->flags == 10);
    CHECK(s.doc.undoLabels().back() == "Set displacement flags");
    REQUIRE(setDisplacementFlags(s.session, sides, Displacement::kFlagNoPhys, false));
    CHECK(document::displacement(s.doc, top).value()->flags == 8);

    const auto undo = s.doc.undoCount();
    const auto report = setDisplacementPower(s.session, sides, 2);
    REQUIRE(report);
    CHECK(report->changed == 1);
    auto disp = *document::displacement(s.doc, top).value();
    CHECK(disp.power == 2);
    CHECK(disp.flags == 8); // Kept.
    CHECK(s.doc.undoCount() == undo + 1);
    CHECK(s.doc.undoLabels().back() == "Set displacement power");

    disp.distances[7] = 4.0;
    s.session.edit("sculpt", [&](document::Transaction& tx) { tx.setDisplacement(top, disp); });
    const auto refused = setDisplacementPower(s.session, sides, 4);
    REQUIRE_FALSE(refused);
    CHECK(std::string(refused.error().what()).find("sculpted or painted") != std::string::npos);
    CHECK(document::displacement(s.doc, top).value()->power == 2);
}
