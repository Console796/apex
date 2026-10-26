#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "geometry/primitives.hpp"
#include "geometry/solid_block.hpp"

#include <cstdlib>
#include <fstream>

using namespace editor_test;
namespace {
ToolView at(DVec3 target) {
    ToolView view;
    view.cursorRay = {target + DVec3{0, 0, 1000}, {0, 0, -1}};
    view.size = {800, 600};
    view.orthographic = true;
    view.cameraForward = {0, 0, -1};
    return view;
}
void drawPrimitive(EditorSession& session, InputSource source = 1) {
    InputEvent e;
    e.source = source; e.kind = InputKind::Press; e.button = PointerButton::Left; e.left = true;
    REQUIRE(session.input(e, at({0, 0, 0})));
    e.kind = InputKind::Release; e.left = false;
    REQUIRE(session.input(e, at({128, 128, 0})));
}
void commit(EditorSession& session) {
    InputEvent e;
    e.source = 1; e.kind = InputKind::KeyDown; e.key = "Enter";
    REQUIRE(session.input(e, {}));
}
struct Scene {
    Document doc{parsed("world { \"id\" \"1\" \"classname\" \"worldspawn\" }")};
    EditorSession session;
    Scene() { session.attach(doc); REQUIRE(session.perform("tool.primitive")); }
};
std::vector<geometry::ConvexPolyhedron> polys(const Document& doc) {
    std::vector<geometry::ConvexPolyhedron> result;
    for (const auto h : nodes(doc)) {
        if (!std::holds_alternative<Handle<Solid>>(h)) continue;
        const auto solid = geometry::solidGeometry(*doc.get(h)->block);
        REQUIRE(solid); result.push_back(solid->poly);
    }
    return result;
}
}
TEST_CASE("primitive parameter changes regenerate preview and typed readout") {
    Scene s;
    drawPrimitive(s.session);
    auto& tool = s.session.activeToolObject();
    REQUIRE(s.session.preview().ghosts().size() == 1);
    CHECK(s.session.preview().ghosts()[0].poly.faces.size() == 10);
    const auto revision = s.session.preview().revision();
    REQUIRE(tool.setParameter("sides", 16));
    CHECK(s.session.preview().revision() > revision);
    CHECK(s.session.preview().ghosts()[0].poly.faces.size() == 18);
    CHECK(tool.readout().parameters[1].value == 16);
    CHECK_FALSE(tool.setParameter("sides", 2));
    CHECK_FALSE(tool.setParameter("sides", 3.5));
    const auto ghost = s.session.preview().ghosts()[0].poly;
    commit(s.session);
    const auto output = polys(s.doc);
    REQUIRE(output.size() == 1);
    CHECK(output[0].vertices.size() == ghost.vertices.size());
    for (const auto v : ghost.vertices)
        CHECK(std::ranges::any_of(output[0].vertices, [&](auto q) { return glm::length(v - q) < 1e-6; }));
}
TEST_CASE("primitive adjust last replaces one undo entry and restores exact prior bytes") {
    Scene s;
    addBox(s.doc, {-128, -128, -64}, {-64, -64, 0});
    const auto before = s.doc.write().value();
    const auto count = s.doc.undoCount();
    drawPrimitive(s.session); commit(s.session);
    auto& tool = s.session.activeToolObject();
    for (auto type : {3, 4, 0, 1}) {
        REQUIRE(tool.setParameter("type", type));
        CHECK(s.doc.undoCount() == count + 1);
    }
    REQUIRE(tool.setParameter("sides", 32));
    CHECK(s.doc.undoCount() == count + 1);
    REQUIRE(s.session.perform("edit.undo"));
    CHECK(s.doc.write().value() == before);
    REQUIRE(s.session.perform("edit.redo"));
    CHECK(polys(s.doc).size() == 2);
}
TEST_CASE("primitive adjustment ends after document edit tool change or new gesture") {
    for (int stop = 0; stop < 5; ++stop) {
        Scene s;
        drawPrimitive(s.session); commit(s.session);
        if (stop == 0) addBox(s.doc, {-64, -64, -64}, {-32, -32, -32});
        if (stop == 1) { REQUIRE(s.session.perform("tool.block")); REQUIRE(s.session.perform("tool.primitive")); }
        if (stop == 2) drawPrimitive(s.session);
        if (stop == 3) { REQUIRE(s.session.perform("edit.undo")); REQUIRE(s.session.perform("edit.redo")); }
        if (stop == 4) { s.session.detach(); s.session.attach(s.doc); }
        const auto before = s.doc.write().value();
        const auto count = s.doc.undoCount();
        REQUIRE(s.session.activeToolObject().setParameter("sides", 12));
        CHECK(s.doc.write().value() == before);
        CHECK(s.doc.undoCount() == count);
    }
}
TEST_CASE("primitive arch and sphere commit as one new VMF group") {
    for (int type : {3, 4}) {
        Scene s;
        auto& tool = s.session.activeToolObject();
        REQUIRE(tool.setParameter("type", type));
        drawPrimitive(s.session);
        const auto ghosts = s.session.preview().ghosts();
        REQUIRE(ghosts.size() == 8);
        commit(s.session);
        const auto output = polys(s.doc);
        REQUIRE(output.size() == 8);
        std::optional<NodeId> group;
        for (const auto h : nodes(s.doc)) {
            if (!std::holds_alternative<Handle<Solid>>(h)) continue;
            const auto parent = s.session.groups().parentGroup(s.doc.get(h)->id);
            REQUIRE(parent);
            if (group) CHECK(parent == group);
            group = parent;
        }
        CHECK(s.session.selection().mode() == SelectionMode::Groups);
        REQUIRE(s.session.selection().ordered().size() == 1);
        CHECK(std::get<NodeId>(s.session.selection().ordered()[0]) == group);
        for (std::size_t i = 0; i < output.size(); ++i) {
            CHECK(output[i].faces.size() == ghosts[i].poly.faces.size());
            for (const auto v : ghosts[i].poly.vertices)
                CHECK(std::ranges::any_of(output[i].vertices, [&](auto q) { return glm::length(v - q) < 1e-6; }));
        }
    }
}
TEST_CASE("primitive VMF export writes every generator for external Hammer checks") {
    Document doc{parsed("world { \"id\" \"1\" \"classname\" \"worldspawn\" }")};
    auto tx = doc.begin("Apex primitive examples");
    for (int type = 0; type < 5; ++type) {
        geometry::PrimitiveBox box;
        box.origin = {type * 384.0, 0, 0};
        box.bounds = {{0, 0, 0}, {256, 256, 128}};
        geometry::PrimitiveParameters p;
        p.type = static_cast<geometry::PrimitiveType>(type);
        const auto shapes = geometry::primitive(box, p);
        REQUIRE(shapes);
        for (const auto& poly : *shapes)
            tx.create(geometry::solidBlock(poly, {"apex/primitive_test"}));
    }
    tx.commit();
    const auto bytes = doc.write();
    REQUIRE(bytes);
    const char* path = std::getenv("APEX_PRIMITIVE_VMF_OUT");
    std::ofstream out(path ? path : APEX_PRIMITIVE_VMF_OUT);
    REQUIRE(out); out << *bytes; REQUIRE(out.good());
}

TEST_CASE("primitive writes single and grouped brushes inside worldspawn through adjustment and redo") {
    for (const int type : {0,1,3}) {
        Scene s;
        REQUIRE(s.session.activeToolObject().setParameter("type", type));
        drawPrimitive(s.session); commit(s.session);
        REQUIRE(s.session.activeToolObject().setParameter("type", type == 3 ? 4 : 2));
        const auto check = [&] {
            for (const auto h : nodes(s.doc)) if (std::holds_alternative<Handle<Solid>>(h))
                CHECK(s.doc.get(h)->parent == s.doc.roots().front());
            const auto saved = s.doc.toVmf();
            REQUIRE(saved);
            REQUIRE(saved->world);
            CHECK(saved->world->solids.size() == polys(s.doc).size());
        };
        check();
        REQUIRE(s.session.perform("edit.undo"));
        REQUIRE(s.session.perform("edit.redo"));
        check();
    }
}
