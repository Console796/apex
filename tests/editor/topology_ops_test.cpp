#include "corpus.hpp"
#include "editor/editor_session.hpp"
#include "editor/topology_ops.hpp"
#include "editor/group_ops.hpp"
#include "geometry/solid_block.hpp"

using namespace editor_test;
namespace {
using Poly = geometry::ConvexPolyhedron;
std::vector<Handle<Solid>> solids(const Document& doc) {
    std::vector<Handle<Solid>> result;
    for (const auto h : nodes(doc)) if (const auto* s = std::get_if<Handle<Solid>>(&h)) result.push_back(*s);
    return result;
}
Poly poly(const Document& doc, Handle<Solid> h) { return geometry::solidGeometry(*doc.get(h)->block).value().poly; }
double volume(const std::vector<Poly>& pieces) {
    double result = 0;
    for (const auto& p : pieces) result += p.volume();
    return result;
}
void select(EditorSession& session, const Document& doc, std::initializer_list<NodeHandle> handles) {
    session.selection().setMode(SelectionMode::Solids);
    for (const auto h : handles) session.selection().add(id(doc, h));
}
void displacement(Document& doc, NodeHandle h) {
    auto b = *doc.get(h)->block;
    b.find("side")->append(kv1::Node::block("dispinfo")).value();
    auto tx = doc.begin("displacement");
    tx.destroy(h);
    tx.create(std::move(b));
    tx.commit();
}
void checkCarve(Document& doc, Handle<Solid> wall, DVec3 min, DVec3 max) {
    auto door = geometry_test::build(geometry_test::boxSides(min, max));
    for (auto& f : door.faces) { f.material = "apex/reveal"; f.uv.shiftU = 37; f.uv.scaleV = 0.25; }
    auto tx = doc.begin("door");
    const auto carver = std::get<Handle<Solid>>(tx.create(geometry::solidBlock(door, {"apex/reveal"})));
    tx.commit();
    // Set deliberately distinct axes so the committed reveal is checked independently.
    auto uv = doc.begin("door axes");
    for (const auto& f : poly(doc, carver).faces) uv.setSideKeyValue(static_cast<SideId>(f.sideId), "uaxis", "[0 1 0 37] 0.5");
    uv.commit();
    const auto before = *doc.write();
    const auto source = poly(doc, wall);
    const auto expected = geometry::subtract(source, poly(doc, carver));
    EditorSession session;
    session.attach(doc);
    select(session, doc, {carver});
    const auto identity = doc.get(wall)->id;
    const auto count = doc.undoCount();
    REQUIRE(carve(session));
    CHECK(doc.get(wall)->id == identity);
    CHECK(poly(doc, wall).volume() == doctest::Approx(expected.front().volume()));
    CHECK(doc.undoCount() == count + 1);
    double total = 0;
    int reveals = 0;
    for (const auto h : solids(doc)) if (h != carver) {
        const auto p = poly(doc, h);
        CHECK(p.isValid());
        total += p.volume();
        for (const auto& f : p.faces) if (f.material == "apex/reveal") {
            ++reveals;
            CHECK(f.uv.shiftU == 37);
            CHECK(f.uv.scaleU == 0.5);
            checkPoint(f.uv.uaxis, {0, 1, 0});
        }
    }
    CHECK(total == doctest::Approx(volume(expected)));
    CHECK(reveals > 0);
    CHECK(poly(doc, carver).volume() == doctest::Approx(door.volume()));
    REQUIRE(doc.undo());
    CHECK(*doc.write() == before);
    REQUIRE(doc.redo());
}
}
TEST_CASE("carve synthetic wall doorway preserves identity volume and reveal attributes") {
    Document doc{parsed()};
    const auto wall = std::get<Handle<Solid>>(addBox(doc, {0, 0, 0}, {256, 32, 128}));
    checkCarve(doc, wall, {96, -16, 0}, {160, 48, 96});
    double total = 0;
    for (const auto h : solids(doc)) total += poly(doc, h).volume();
    CHECK(total == doctest::Approx(256.*32*128 - 64.*32*96 + 64.*64*96));
}
TEST_CASE("carve corpus wall doorway") {
    const auto paths = corpusPaths();
    if (paths.empty()) { MESSAGE("APEX_P2CE_DIR unset; skipping carve corpus wall"); return; }
    for (const auto& path : paths) {
        Document source{readCorpus(path)};
        for (const auto h : solids(source)) {
            const auto geometry = geometry::solidGeometry(*source.get(h)->block);
            if (!geometry || hasDisplacement(*source.get(h)->block)) continue;
            const auto bounds = geometry->poly.bounds();
            const auto extent = bounds.max - bounds.min;
            if (extent.x < 64 || extent.y < 8 || extent.z < 64) continue;
            Document doc{parsed()};
            auto tx = doc.begin("wall");
            const auto wall = std::get<Handle<Solid>>(tx.create(source.exportSubtree(source.get(h)->id)));
            tx.commit();
            checkCarve(doc, wall, bounds.min + extent * DVec3{0.3, -0.1, -0.1},
                       bounds.min + extent * DVec3{0.7, 1.1, 0.8});
            return;
        }
    }
    FAIL("No suitable corpus wall");
}
TEST_CASE("hollow plus and minus sixteen produces six disjoint walls in a new group") {
    for (const double thickness : {16.0, -16.0}) {
        Document doc{parsed()};
        const auto box = std::get<Handle<Solid>>(addBox(doc, {0, 0, 0}, {128, 128, 128}));
        EditorSession session;
        session.attach(doc);
        select(session, doc, {box});
        const auto before = *doc.write();
        const auto count = doc.undoCount();
        REQUIRE(hollow(session, thickness));
        const auto walls = solids(doc);
        REQUIRE(walls.size() == 6);
        CHECK(doc.get(box));
        CHECK(doc.undoCount() == count + 1);
        CHECK(session.selection().mode() == SelectionMode::Groups);
        REQUIRE(session.selection().ordered().size() == 1);
        const auto group = std::get<NodeId>(session.selection().ordered()[0]);
        double total = 0;
        for (std::size_t i = 0; i < walls.size(); ++i) {
            CHECK(session.groups().parentGroup(doc.get(walls[i])->id) == group);
            const auto p = poly(doc, walls[i]);
            total += p.volume();
            for (std::size_t j = i + 1; j < walls.size(); ++j)
                CHECK(volume(geometry::subtract(p, poly(doc, walls[j]))) == doctest::Approx(p.volume()));
        }
        const double expected = thickness > 0 ? 128.*128*128 - 96.*96*96 : 160.*160*160 - 128.*128*128;
        CHECK(total == doctest::Approx(expected));
        REQUIRE(doc.undo());
        CHECK(*doc.write() == before);
        REQUIRE(doc.redo());
    }
}
TEST_CASE("merge convex touching boxes does not ask and preserves first identity") {
    Document doc{parsed()};
    const auto a = addBox(doc, {0, 0, 0}, {64, 64, 64});
    const auto b = addBox(doc, {64, 0, 0}, {128, 64, 64});
    EditorSession session;
    session.attach(doc);
    select(session, doc, {b, a});
    session.confirmMerge = [](const auto&) { FAIL("Convex merge asked"); return false; };
    const auto count = doc.undoCount();
    REQUIRE(session.perform("edit.merge"));
    CHECK(solids(doc).size() == 1);
    CHECK(doc.get(b));
    CHECK_FALSE(doc.get(a));
    CHECK(doc.undoCount() == count + 1);
    REQUIRE(doc.undo());
    CHECK(solids(doc).size() == 2);
}
TEST_CASE("merge L shape confirmation cancel commit preference and ghost volume") {
    for (const bool preference : {true, false}) {
        Document doc{parsed()};
        const auto a = addBox(doc, {0, 0, 0}, {128, 64, 64});
        const auto b = addBox(doc, {0, 64, 0}, {64, 128, 64});
        EditorSession session;
        session.attach(doc);
        select(session, doc, {a, b});
        const auto plan = planMerge(session);
        REQUIRE(plan);
        CHECK_FALSE(plan->convex());
        CHECK(plan->addedVolume == doctest::Approx(64.*64*64/2));
        CHECK(plan->addedVolume == doctest::Approx(volume(plan->ghosts)));
        const auto before = *doc.write();
        const auto count = doc.undoCount();
        int asks = 0;
        session.confirmMerge = [&](const auto& p) {
            ++asks;
            CHECK(session.preview().ghosts().size() == p.ghosts.size());
            CHECK(*doc.write() == before);
            return false;
        };
        REQUIRE(session.perform("edit.merge"));
        CHECK(asks == 1);
        CHECK(*doc.write() == before);
        CHECK(doc.undoCount() == count);
        CHECK(session.preview().empty());
        session.confirmNonConvexMerge = preference;
        session.confirmMerge = [&](const auto&) { ++asks; return true; };
        REQUIRE(session.perform("edit.merge"));
        CHECK(asks == (preference ? 2 : 1));
        CHECK(doc.undoCount() == count + 1);
        REQUIRE(solids(doc).size() == 1);
        CHECK(poly(doc, std::get<Handle<Solid>>(a)).volume() == doctest::Approx(plan->hull.volume()));
        REQUIRE(doc.undo());
        CHECK(*doc.write() == before);
    }
}
TEST_CASE("topology commands refuse displacement carvers targets hollow and merge atomically") {
    Document doc{parsed()};
    const auto a = addBox(doc, {0, 0, 0}, {128, 128, 128});
    const auto b = addBox(doc, {32, -16, 0}, {64, 144, 96});
    displacement(doc, a);
    auto displaced = solids(doc).back();
    for (const auto h : solids(doc)) if (hasDisplacement(*doc.get(h)->block)) displaced = h;
    EditorSession session;
    session.attach(doc);
    select(session, doc, {displaced});
    const auto before = *doc.write();
    const auto count = doc.undoCount();
    const auto c = carve(session), h = hollow(session, 16);
    REQUIRE_FALSE(c); REQUIRE_FALSE(h);
    CHECK(c.error().message.find("Displacement") != std::string::npos);
    session.selection().add(id(doc, b));
    REQUIRE_FALSE(planMerge(session));
    session.selection().select(id(doc, b));
    const auto target = carve(session);
    REQUIRE_FALSE(target);
    CHECK(target.error().message.find("displacement target") != std::string::npos);
    CHECK(*doc.write() == before);
    CHECK(doc.undoCount() == count);
}

TEST_CASE("topology VMF export contains Apex authored carve hollow and merge results") {
    Document doc{parsed(R"(world { "id" "1" "classname" "worldspawn" })")};
    const auto world = worldHandle(doc);
    const auto box = [&](DVec3 min, DVec3 max) {
        auto tx = doc.begin("export box");
        const auto h = tx.create(geometry::solidBlock(
            geometry_test::build(geometry_test::boxSides(min, max)), {"dev/dev_measuregeneric01"}), world);
        tx.commit();
        return h;
    };
    const auto wall = box({0, 0, 0}, {256, 32, 128});
    const auto door = box({96, -16, 0}, {160, 48, 96});
    EditorSession session;
    session.attach(doc);
    select(session, doc, {door});
    REQUIRE(carve(session));
    CHECK(doc.get(wall));
    const auto shell = box({320, 0, 0}, {448, 128, 128});
    session.selection().select(id(doc, shell));
    REQUIRE(hollow(session, 16));
    const auto a = box({512, 0, 0}, {576, 64, 64});
    const auto b = box({576, 0, 0}, {640, 64, 64});
    session.selection().clear();
    select(session, doc, {a, b});
    REQUIRE(session.perform("edit.merge"));
    const auto bytes = doc.write();
    REQUIRE(bytes);
    const char* path = std::getenv("APEX_TOPOLOGY_VMF_OUT");
    const auto output = path ? std::filesystem::path(path) :
        std::filesystem::path(APEX_PRIMITIVE_VMF_OUT).replace_filename("topology.vmf");
    std::ofstream out(output);
    REQUIRE(out);
    out << *bytes;
    REQUIRE(out.good());
    CHECK(vmf::parse(*bytes));
}
