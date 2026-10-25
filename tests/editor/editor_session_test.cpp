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

TEST_CASE("EditorSession surface hover is independent of selection mode and cached by face") {
    Document doc(parsed());
    const auto box = addBox(doc, {0, 0, 0}, {10, 20, 30});
    addBlock(doc, R"(entity { "classname" "info_target" "origin" "100 0 0" })");
    EditorSession session;
    session.attach(doc);
    int notifications = 0;
    session.observe([&] { ++notifications; });
    for (const auto mode : {SelectionMode::Groups, SelectionMode::Objects, SelectionMode::Solids,
                           SelectionMode::Faces}) {
        session.selection().setMode(mode);
        session.clearHover();
        session.pointerHover({{-100, 5, 5}, {1, 0, 0}});
        auto plane = session.resolvedWorkPlane();
        REQUIRE(plane);
        CHECK(plane->source == WorkPlaneSource::Hovered);
        checkPoint(plane->plane.normal, {-1, 0, 0});
        checkPoint(plane->plane.origin, {0, 0, 0}); // The world origin projected onto x = 0 (0.2 D9).
        REQUIRE(plane->plane.bounds);
        const auto before = notifications;
        session.pointerHover({{-100, 6, 6}, {1, 0, 0}});
        CHECK(notifications == before);
        // Same drawable and (except in Faces mode) same target, different face.
        session.pointerHover({{5, 5, 100}, {0, 0, -1}});
        plane = session.resolvedWorkPlane();
        REQUIRE(plane);
        checkPoint(plane->plane.normal, {0, 0, 1});
        CHECK(notifications == before + 1);
        session.pointerHover({{100, 0, 100}, {0, 0, -1}});
        CHECK_FALSE(session.resolvedWorkPlane());
        session.pointerHover({{-100, 5, 5}, {1, 0, 0}});
        session.pointerHover({{-100, 500, 500}, {1, 0, 0}});
        CHECK_FALSE(session.resolvedWorkPlane());
    }
    session.pointerHover({{-100, 5, 5}, {1, 0, 0}});
    session.clearHover();
    CHECK_FALSE(session.resolvedWorkPlane());
    session.pointerHover({{-100, 5, 5}, {1, 0, 0}});
    session.setViewState({});
    CHECK_FALSE(session.resolvedWorkPlane());
    // Geometry edits invalidate the node/side cache, even if the IDs survive.
    session.pointerHover({{-100, 5, 5}, {1, 0, 0}});
    auto tx = doc.begin("Move box");
    auto geometry = geometry::solidGeometry(*doc.get(box)->block);
    REQUIRE(geometry);
    translate(*geometry, {2, 0, 0});
    tx.setSolidGeometry(std::get<Handle<Solid>>(box), *geometry);
    tx.commit();
    CHECK_FALSE(session.resolvedWorkPlane());
    session.pointerHover({{-100, 5, 5}, {1, 0, 0}});
    REQUIRE(session.resolvedWorkPlane());
    checkPoint(session.resolvedWorkPlane()->plane.origin, {2, 0, 0});
}

TEST_CASE("EditorSession pinned planes survive hover and Esc unpins before clearing selection") {
    Document doc(parsed());
    const auto box = addBox(doc, {0, 0, 0}, {10, 20, 30});
    EditorSession session;
    session.attach(doc);
    session.selectNode(id(doc, box));
    const auto history = doc.undoCount();
    const auto written = doc.write().value();
    session.pointerHover({{-100, 5, 5}, {1, 0, 0}});
    CHECK(session.perform("workplane.pin"));
    session.pointerHover({{5, 5, 100}, {0, 0, -1}});
    auto plane = session.resolvedWorkPlane();
    REQUIRE(plane);
    CHECK(plane->source == WorkPlaneSource::Pinned);
    checkPoint(plane->plane.normal, {-1, 0, 0});
    session.clearHover();
    CHECK(session.workPlane().pinned());
    const auto escape = session.keymap().lookup(KeyChord::parse("Escape").value());
    REQUIRE(escape);
    CHECK(session.perform(*escape));
    CHECK_FALSE(session.workPlane().pinned());
    CHECK_FALSE(session.resolvedWorkPlane());
    CHECK_FALSE(session.selection().ordered().empty());
    CHECK(session.perform(*escape));
    CHECK(session.selection().ordered().empty());
    CHECK(doc.undoCount() == history);
    CHECK(doc.write().value() == written);
}

TEST_CASE("EditorSession grid actions walk the ladder clamp and notify only for changed sizes") {
    Document doc(parsed());
    EditorSession session;
    session.attach(doc);
    CHECK(session.gridSize() == 64.0);
    int notifications = 0;
    session.observe([&] { ++notifications; });
    const auto smaller = session.keymap().lookup(KeyChord::parse("[").value());
    const auto larger = session.keymap().lookup(KeyChord::parse("]").value());
    REQUIRE(smaller);
    REQUIRE(larger);
    for (double value = 32; value >= 0.125; value *= 0.5) {
        CHECK(session.perform(*smaller));
        CHECK(session.gridSize() == value);
    }
    CHECK(notifications == 9);
    session.perform(*smaller);
    CHECK(notifications == 9);
    for (double value = 0.25; value <= 1024; value *= 2) {
        CHECK(session.perform(*larger));
        CHECK(session.gridSize() == value);
    }
    CHECK(notifications == 22);
    session.perform(*larger);
    CHECK(notifications == 22);
    session.setGridSize(0.01);
    CHECK(session.gridSize() == 0.125);
    session.setGridSize(48);
    CHECK(session.gridSize() == 64);
    session.setGridSize(5000);
    CHECK(session.gridSize() == 1024);
    CHECK(doc.undoCount() == 0);
}

TEST_CASE("EditorSession axis planes use selection bounds and survive undo redo") {
    Document doc(parsed());
    const auto group = addBlock(doc, "group {}");
    const auto box = addBox(doc, {10, 20, 30}, {30, 60, 90});
    const auto entity = addBlock(doc, R"(entity { "origin" "100 120 140" })");
    auto tx = doc.begin("Group box");
    tx.setEditor(box, EditorEdit::setGroup(doc.get(group)->block->find("id")->asInt().value()));
    tx.commit();
    EditorSession session;
    session.attach(doc);
    session.selectNode(id(doc, group));
    for (const auto& [action, normal] : {
             std::pair{"workplane.axis_x", DVec3{1, 0, 0}},
             std::pair{"workplane.axis_y", DVec3{0, 1, 0}},
             std::pair{"workplane.axis_z", DVec3{0, 0, 1}}}) {
        CHECK(session.perform(action));
        const auto plane = session.resolvedWorkPlane();
        REQUIRE(plane);
        CHECK(plane->source == WorkPlaneSource::Pinned);
        CHECK_FALSE(plane->plane.bounds);
        checkPoint(plane->plane.normal, normal);
        checkPoint(plane->plane.origin, {20, 40, 60});
    }
    session.setGridSize(0.25);
    session.edit("Delete box", [&](Transaction& edit) { edit.destroy(box); });
    for (const auto* action : {"edit.undo", "edit.redo"}) {
        CHECK(session.perform(action));
        REQUIRE(session.resolvedWorkPlane());
        checkPoint(session.resolvedWorkPlane()->plane.origin, {20, 40, 60});
        CHECK(session.workPlane().pinned());
        CHECK(session.gridSize() == 0.25);
    }
    session.selectNode(id(doc, entity));
    session.perform("workplane.axis_x");
    checkPoint(session.resolvedWorkPlane()->plane.origin, {100, 120, 140});
    session.selection().clear();
    session.perform("workplane.axis_y");
    checkPoint(session.resolvedWorkPlane()->plane.origin, {0, 0, 0});
    session.pinAxisWorkPlane(Axis::Z, {1, 2, 3});
    checkPoint(session.resolvedWorkPlane()->plane.origin, {1, 2, 3});
    session.detach();
    CHECK_FALSE(session.resolvedWorkPlane());
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

TEST_CASE("EditorSession history jumps restore selection like undo and redo") {
    Document doc(parsed());
    const auto box = addBox(doc, {0, 0, 0}, {16, 16, 16});
    const auto boxId = id(doc, box);
    EditorSession session;
    CHECK_FALSE(session.jumpHistory(0));
    session.attach(doc);
    session.selectNode(boxId);
    const auto start = doc.undoCount();
    const auto before = doc.write().value();
    REQUIRE(session.duplicateSelection());
    const auto duplicate = session.selection().ordered();
    const auto middle = doc.write().value();
    REQUIRE(session.deleteSelection());
    const auto after = doc.write().value();
    REQUIRE(session.jumpHistory(start));
    CHECK(doc.write().value() == before);
    CHECK(session.selection().ordered() == std::vector<Target>{boxId});
    REQUIRE(session.jumpHistory(start + 1));
    CHECK(doc.write().value() == middle);
    CHECK(session.selection().ordered() == duplicate);
    REQUIRE(session.jumpHistory(start + 2));
    CHECK(doc.write().value() == after);
    CHECK(session.selection().ordered().empty());
    CHECK_FALSE(session.jumpHistory(start + 3));
    session.selectNode(boxId);
    REQUIRE(session.jumpHistory(start + 2));
    CHECK(session.selection().ordered() == std::vector<Target>{boxId});
}

TEST_CASE("Outliner moves preserve ids, membership, order and exact undo") {
    Document doc(parsed("world { id 1 classname worldspawn group { id 2 } group { id 3 } } entity { id 4 classname func_detail } entity { id 5 classname info_target }"));
    const auto world = *worldHandle(doc);
    const auto a = id(doc, addBox(doc, {0,0,0}, {8,8,8}, world));
    const auto b = id(doc, addBox(doc, {16,0,0}, {24,8,8}, world));
    std::vector<NodeId> groups, entities;
    for (auto h : nodes(doc)) {
        if (std::holds_alternative<Handle<Group>>(h)) groups.push_back(id(doc,h));
        if (doc.get(h)->block->key() == "entity") entities.push_back(id(doc,h));
    }
    EditorSession session;
    session.attach(doc);
    const auto original = *doc.write();
    REQUIRE(session.moveNodesInto({a}, groups[0]));
    CHECK(session.groups().parentGroup(a) == groups[0]);
    REQUIRE(session.perform("edit.undo"));
    CHECK(*doc.write() == original);
    REQUIRE(session.moveNodesInto({b,a}, entities[0]));
    CHECK(doc.get(*doc.handle(a))->parent == entities[0]);
    CHECK(doc.get(*doc.handle(b))->parent == entities[0]);
    const auto children = doc.get(*doc.handle(entities[0]))->children;
    CHECK(std::find(children.begin(),children.end(),a) < std::find(children.begin(),children.end(),b));
    REQUIRE(session.perform("edit.undo"));
    CHECK(*doc.write() == original);
    REQUIRE(session.moveNodesInto({groups[0]}, groups[1]));
    CHECK(session.groups().parentGroup(groups[0]) == groups[1]);
    const auto nested = *doc.write();
    CHECK_FALSE(session.moveNodesInto({groups[1]}, groups[0]));
    CHECK_FALSE(session.moveNodesInto({groups[0]}, groups[0]));
    CHECK_FALSE(session.moveNodesInto({entities[1]}, entities[0]));
    CHECK_FALSE(session.moveNodesInto({groups[0]}, entities[0]));
    CHECK(*doc.write() == nested);
    REQUIRE(session.perform("edit.undo"));
    CHECK(*doc.write() == original);
    REQUIRE(session.moveNodesInto({a}, entities[0]));
    REQUIRE(session.moveNodesInto({a}, id(doc,world)));
    CHECK(doc.get(*doc.handle(a))->parent == id(doc,world));
    CHECK_FALSE(session.groups().parentGroup(a));
}

TEST_CASE("Outliner rename and single object visibility preserve selection and undo") {
    Document doc(parsed("world { id 1 classname worldspawn } entity { id 2 classname info_target }"));
    const auto solid = id(doc, addBox(doc, {0,0,0}, {8,8,8}, worldHandle(doc)));
    NodeId entity = 0;
    for (auto h : nodes(doc)) if (doc.get(h)->block->key() == "entity") entity = id(doc,h);
    EditorSession session;
    session.attach(doc);
    session.selection().add(solid);
    const auto selection = session.selection().toBlob();
    const auto original = *doc.write();
    REQUIRE(session.renameEntity(entity, "destination"));
    CHECK(doc.get(*doc.handle(entity))->block->find("targetname")->decodedValue() == "destination");
    REQUIRE(session.renameEntity(entity, ""));
    CHECK(doc.get(*doc.handle(entity))->block->find("targetname") == nullptr);
    REQUIRE(session.perform("edit.undo"));
    CHECK(doc.get(*doc.handle(entity))->block->find("targetname")->decodedValue() == "destination");
    REQUIRE(session.perform("edit.undo"));
    CHECK(*doc.write() == original);
    REQUIRE(session.setNodeHidden({solid}, true));
    CHECK(session.nodeHidden(solid));
    CHECK(session.selection().toBlob() == selection);
    const auto hidden = *doc.write();
    CHECK_FALSE(session.moveNodesInto({solid}, entity));
    REQUIRE(session.setNodeHidden({solid}, false));
    CHECK_FALSE(session.nodeHidden(solid));
    CHECK(*doc.write() == original);
    CHECK(session.selection().toBlob() == selection);
    REQUIRE(session.perform("edit.undo"));
    CHECK(*doc.write() == hidden);
    REQUIRE(session.perform("edit.undo"));
    CHECK(*doc.write() == original);
}

TEST_CASE("Outliner visibility splits shared wrappers and toggles empty groups") {
    Document doc(parsed("world { id 1 hidden { solid { id 2 } solid { id 3 } } group { id 4 } }"));
    std::vector<NodeId> solids;
    NodeId group = 0;
    for (const auto h : nodes(doc)) {
        if (std::holds_alternative<Handle<Solid>>(h)) solids.push_back(id(doc,h));
        if (std::holds_alternative<Handle<Group>>(h)) group = id(doc,h);
    }
    EditorSession session;
    session.attach(doc);
    const auto original = *doc.write();
    REQUIRE(session.setNodeHidden({solids[0]}, false));
    CHECK_FALSE(session.nodeHidden(solids[0]));
    CHECK(session.nodeHidden(solids[1]));
    REQUIRE(session.perform("edit.undo"));
    CHECK(*doc.write() == original);
    REQUIRE(session.setNodeHidden({group}, true));
    CHECK(session.nodeHidden(group));
    REQUIRE(session.setNodeHidden({group}, false));
    CHECK_FALSE(session.nodeHidden(group));
    CHECK(*doc.write() == original);
}

TEST_CASE("EditorSession applies materials to sides and to the face under a drop ray") {
    Document doc(parsed());
    const auto box = addBox(doc, {0, 0, 0}, {10, 20, 30});
    EditorSession session;
    session.attach(doc);
    std::vector<SideId> sides;
    for (const auto& child : doc.get(box)->block->children())
        if (child.kind() == kv1::NodeKind::Block && ciEquals(child.key(), "side"))
            sides.push_back(static_cast<SideId>(*child.find("id")->asInt()));
    REQUIRE(sides.size() == 6);
    const auto material = [&](SideId side) {
        const auto location = doc.side(side);
        std::size_t face = 0;
        for (const auto& child : doc.get(location->solid)->block->children()) {
            if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
            if (face++ != location->faceIndex) continue;
            const auto* key = child.find("material");
            return key ? key->decodedValue().value_or("") : std::string{};
        }
        return std::string{};
    };
    const auto before = material(sides[0]);
    const auto undo = doc.undoCount();
    CHECK(session.applyMaterial(std::span(sides).first(2), "dev/red") == 2);
    CHECK(doc.undoCount() == undo + 1);
    CHECK(doc.undoLabels().back() == "Apply Material");
    CHECK(material(sides[0]) == "dev/red");
    CHECK(material(sides[1]) == "dev/red");
    CHECK(material(sides[2]) == before);
    CHECK(session.applyMaterial(std::span(sides).first(2), "DEV/RED") == 0); // Already set: no undo entry.
    CHECK(doc.undoCount() == undo + 1);
    doc.undo();
    CHECK(material(sides[0]) == before);

    const auto hit = session.picks().first({{-100, 5, 5}, {1, 0, 0}});
    REQUIRE(hit);
    REQUIRE(hit->side);
    CHECK(session.applyMaterialAt({{-100, 5, 5}, {1, 0, 0}}, "dev/blue"));
    CHECK(material(*hit->side) == "dev/blue");
    for (const auto side : sides) if (side != *hit->side) CHECK(material(side) != "dev/blue");
    const auto afterDrop = doc.undoCount();
    CHECK_FALSE(session.applyMaterialAt({{-100, 500, 5}, {1, 0, 0}}, "dev/blue")); // Misses the box.
    CHECK(doc.undoCount() == afterDrop);
}
