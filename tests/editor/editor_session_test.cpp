#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "viewport/pick_math.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace editor_test;

namespace {
void replay(Document& doc) {
    // A deterministic editing area outside the map, exercised through a scripted
    // camera. Corpus contents remain loaded, indexed and observed throughout.
    const auto front = addBox(doc, {200000, 199990, 199990}, {200010, 200010, 200010});
    const auto back = addBox(doc, {200020, 199990, 199990}, {200030, 200010, 200010});
    const auto frontId = id(doc, front), backId = id(doc, back);
    EditorSession session;
    session.attach(doc);
    session.perform("selection.mode.solids");
    const auto history = doc.undoCount();
    const auto written = doc.write().value();
    viewport::Camera camera;
    camera.position = {199900, 200000, 200000};
    camera.fovYDegrees = 90;
    const auto ray = viewport::pixelRay(camera, 1, 100, 100, 200, 200);
    session.pointerHover(ray);
    CHECK(session.hoverTarget() == Target{frontId});
    session.pointerClick(ray);
    CHECK(session.selection().ordered() == std::vector<Target>{frontId});
    session.pointerClick(ray, {}, true);
    CHECK(session.selection().ordered() == std::vector<Target>{backId});
    session.pointerClick(ray, {.shift = true});
    CHECK(session.selection().ordered().size() == 2);
    session.pointerClick(ray, {.ctrl = true});
    CHECK(session.selection().ordered() == std::vector<Target>{backId});
    CHECK(doc.undoCount() == history);
    CHECK(doc.write().value() == written);

    // Both brushes are enclosed; a smaller rectangle touches without enclosing.
    camera.projection = viewport::ProjectionKind::Orthographic;
    camera.orthoHeight = 100;
    const auto planes = viewport::rectFrustum(camera, 1, 80, 80, 120, 120, 200, 200);
    session.marquee(planes);
    CHECK(session.selection().ordered().size() == 2);
    const auto partial = viewport::rectFrustum(camera, 1, 95, 95, 105, 105, 200, 200);
    session.marquee(partial);
    const auto enclosed = session.selection().ordered().size();
    session.perform("selection.marquee_toggle");
    session.marquee(partial);
    CHECK(session.selection().ordered().size() > enclosed);

    session.selectNode(frontId);
    session.edit("Delete selected brush", [&](Transaction& tx) { tx.destroy(front); });
    CHECK(session.selection().ordered().empty());
    CHECK_FALSE(doc.handle(frontId));
    CHECK(session.perform("edit.undo"));
    CHECK(doc.handle(frontId));
    CHECK(*doc.handle(frontId) != front);
    CHECK(session.selection().ordered() == std::vector<Target>{frontId});
    CHECK(session.perform("edit.redo"));
    CHECK(session.selection().ordered().empty());
    CHECK_FALSE(doc.handle(frontId));
}
}

TEST_CASE("EditorSession headless camera replay cycles marquees and restores deleted selection") {
    Document doc(parsed());
    replay(doc);
}

TEST_CASE("EditorSession actions attachment and edit metadata preserve selection policy") {
    Document doc(parsed(R"(entity { "id" "1" "origin" "0 0 0" })"));
    EditorSession session;
    CHECK_FALSE(session.perform("selection.all"));
    session.attach(doc);
    CHECK(session.perform("selection.all"));
    CHECK(session.selection().ordered().size() == 1);
    CHECK(session.perform("selection.invert"));
    CHECK(session.selection().ordered().empty());
    session.perform("selection.all");
    session.perform("selection.ignore_groups");
    CHECK(session.selection().ignoreGroups());
    session.perform("selection.none");
    CHECK(session.selection().ordered().empty());
    CHECK_FALSE(session.perform("edit.delete"));
    CHECK_FALSE(session.perform("not.an.action"));
    for (const auto& name : {"groups", "objects", "solids", "faces"}) {
        CHECK(session.perform(std::string("selection.mode.") + name));
    }
    CHECK(session.selection().mode() == SelectionMode::Faces);
    CHECK(doc.undoCount() == 0);
    session.setPreset(blenderPreset());
    CHECK(session.keymap().lookup(KeyChord::parse("A").value()) == "selection.all");
    auto created = addBox(doc, {0, 0, 0}, {10, 10, 10});
    CHECK(session.picks().size() == 2);
    session.perform("selection.mode.solids");
    session.selectNode(id(doc, created));
    auto tx = doc.begin("External delete");
    tx.destroy(created);
    tx.commit();
    CHECK(session.selection().ordered().empty());
    CHECK(session.picks().size() == 1);
    Document second(parsed());
    session.attach(second);
    CHECK(session.picks().size() == 0);
    addBox(doc, {20, 20, 20}, {30, 30, 30});
    CHECK(session.picks().size() == 0);
    session.detach();
}

TEST_CASE("EditorSession P2CE corpus scripted camera replay") {
    namespace fs = std::filesystem;
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping EditorSession corpus replay");
        return;
    }
    const fs::path root = fs::path(install) / "sdk_content/maps";
    REQUIRE(fs::is_directory(root));
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") paths.push_back(entry.path());
    }
    std::ranges::sort(paths);
    REQUIRE_FALSE(paths.empty());
    std::ifstream stream(paths.front(), std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    auto file = vmf::parse(bytes);
    if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
    REQUIRE(file);
    Document doc(*file);
    // Select original corpus geometry too, with an independent plane-slab oracle.
    std::vector<std::pair<NodeId, geometry::ConvexPolyhedron>> solids;
    double maxX = -std::numeric_limits<double>::infinity();
    for (const auto h : nodes(doc)) {
        const auto* n = doc.get(h);
        if (!std::holds_alternative<Handle<Solid>>(h) || !isVisible(doc, n->id)) continue;
        if (auto geometry = geometry::solidGeometry(*n->block)) {
            maxX = std::max(maxX, geometry->poly.bounds().max.x);
            solids.emplace_back(n->id, std::move(geometry->poly));
        }
    }
    REQUIRE_FALSE(solids.empty());
    viewport::Camera camera;
    camera.position = solids.front().second.center();
    camera.position.x = maxX + 128;
    camera.angles = {0, 180, 0};
    const auto ray = viewport::pixelRay(camera, 1, 100, 100, 200, 200);
    NodeId expected = 0;
    double nearest = std::numeric_limits<double>::infinity();
    for (const auto& [node, poly] : solids) {
        double enter = 0, exit = std::numeric_limits<double>::infinity();
        for (const auto& face : poly.faces) {
            const double distance = face.plane.signedDistance(ray.origin);
            const double direction = glm::dot(face.plane.normal, ray.dir);
            if (std::abs(direction) < 1e-10) {
                if (distance > 1e-6) { exit = -1; break; }
            } else if (direction < 0) enter = std::max(enter, -distance / direction);
            else exit = std::min(exit, -distance / direction);
        }
        if (enter <= exit && enter < nearest) { nearest = enter; expected = node; }
    }
    REQUIRE(expected != 0);
    {
        EditorSession session;
        session.attach(doc);
        session.perform("selection.mode.solids");
        session.pointerClick(ray);
        CHECK(session.selection().ordered() == std::vector<Target>{expected});
    }
    replay(doc);
}

TEST_CASE("EditorSession group hover retains the actual drawable and clears on view changes") {
    Document doc(parsed());
    const auto group = addBlock(doc, "group {}");
    const auto front = addBox(doc, {0, -10, -10}, {10, 10, 10});
    const auto back = addBox(doc, {20, -10, -10}, {30, 10, 10});
    auto tx = doc.begin("Group brushes");
    const auto groupId = doc.get(group)->block->find("id")->asInt();
    REQUIRE(groupId);
    tx.setEditor(front, EditorEdit::setGroup(*groupId));
    tx.setEditor(back, EditorEdit::setGroup(*groupId));
    tx.commit();
    EditorSession session;
    session.attach(doc);
    session.pointerHover({{-100, 0, 0}, {1, 0, 0}});
    CHECK(session.hoverTarget() == Target{id(doc, group)});
    CHECK(session.hoverDrawable() == id(doc, front));
    session.pointerHover({{100, 0, 0}, {-1, 0, 0}});
    CHECK(session.hoverTarget() == Target{id(doc, group)});
    CHECK(session.hoverDrawable() == id(doc, back));
    session.setViewState({std::unordered_set<NodeId>{id(doc, front)}});
    CHECK_FALSE(session.hoverTarget());
    CHECK_FALSE(session.hoverDrawable());
    session.pointerHover({{25, 0, 100}, {0, 0, -1}});
    CHECK_FALSE(session.hoverTarget());
    session.setViewState({});
    session.selectNode(id(doc, group));
    CHECK(session.selection().ordered() == std::vector<Target>{id(doc, group)});
    session.perform("selection.mode.solids");
    session.selectNode(id(doc, group));
    CHECK(session.selection().ordered().size() == 2);
    session.perform("selection.mode.faces");
    session.selectNode(id(doc, group));
    CHECK(session.selection().ordered().size() == 12);
}
