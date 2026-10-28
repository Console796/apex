#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/placement_item.hpp"

#include <algorithm>
#include <tuple>

using namespace editor_test;

namespace {
assets::EntityClassTable itemClasses() {
    assets::EntityClassTable table;
    assets::EntityClass cls;
    cls.name = "prop_static";
    cls.type = "PointClass";
    cls.mins = {-3, -5, -7};
    cls.maxs = {11, 13, 17};
    table.classes[cls.name] = cls;
    cls.name = "prop_dynamic";
    table.classes[cls.name] = cls;
    cls.name = "brush";
    cls.type = "SolidClass";
    table.classes[cls.name] = cls;
    return table;
}
DVec3 entityVector(const kv1::Node& entity, const char* key) {
    const auto* value = entity.find(key);
    REQUIRE(value);
    const auto vector = value->asVec3();
    REQUIRE(vector);
    return {(*vector)[0], (*vector)[1], (*vector)[2]};
}
// Rounded so rotation noise at 1e-15 cannot reorder the corners.
bool byPosition(DVec3 a, DVec3 b) {
    const auto key = [](DVec3 v) { return std::tuple{std::round(v.x * 1e3), std::round(v.y * 1e3), std::round(v.z * 1e3)}; };
    return key(a) < key(b);
}
// The eight corners of the placed box, computed here and not by placedBox().
std::vector<DVec3> expectedCorners(const DVec3& origin, const std::optional<DVec3>& angles, const AABB& box) {
    const auto basis = angles ? basisFromAngles(*angles) : DMat3{1.0};
    std::vector<DVec3> corners;
    for (unsigned c = 0; c < 8; ++c)
        corners.push_back(origin + basis * DVec3{c & 1 ? box.max.x : box.min.x, c & 2 ? box.max.y : box.min.y,
                                                 c & 4 ? box.max.z : box.min.z});
    std::ranges::sort(corners, byPosition);
    return corners;
}
std::vector<DVec3> ghostCorners(const Preview& preview) {
    REQUIRE(preview.ghosts().size() == 1);
    auto corners = preview.ghosts().front().poly.vertices;
    std::ranges::sort(corners, byPosition);
    return corners;
}
void checkCorners(const std::vector<DVec3>& actual, const std::vector<DVec3>& expected) {
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) checkPoint(actual[i], expected[i]);
}
}

TEST_CASE("a model item's ghost is the box the click then places, on a wall, the floor and the work plane") {
    const AABB bounds{{-10, -20, -30}, {10, 20, 30}};
    for (const bool align : {false, true}) {
        for (const Ray ray : {Ray{{100, 3, 7}, {-1, 0, 0}}, Ray{{9, 11, 100}, {0, 0, -1}},
                              Ray{{300, 3, 100}, {-1, -0.01, -0.3}}}) {
            INFO("align=" << align << " dir=" << ray.dir.x << "," << ray.dir.y << "," << ray.dir.z);
            Document doc(parsed());
            addBox(doc, {-16, -64, -64}, {0, 64, 64});
            EditorSession session;
            session.attach(doc);
            session.setEntityClasses(itemClasses());
            session.setEntityBoundsProvider([&](const kv1::Node&) -> std::optional<EntityPickShape> {
                return EntityPickShape{{}, 0, {bounds.min, bounds.max}};
            });
            session.setAlignToSurface(align);
            session.setPlacementItem(ModelItem{"models/props/barrel.mdl"});
            REQUIRE(session.perform("tool.entity"));
            const auto history = doc.undoCount();
            session.pointerHover(ray);
            CHECK(doc.undoCount() == history); // Hovering edits nothing.
            REQUIRE_FALSE(session.preview().empty());
            const auto ghost = ghostCorners(session.preview());
            session.pointerClick(ray);
            REQUIRE(doc.undoCount() == history + 1);
            const auto id = std::get<NodeId>(*session.selection().active());
            const auto& entity = *doc.get(*doc.handle(id))->block;
            CHECK(entity.find("classname")->decodedValue() == "prop_static");
            CHECK(entity.find("model")->decodedValue() == "models/props/barrel.mdl");
            std::optional<DVec3> angles;
            if (align) angles = entityVector(entity, "angles");
            checkCorners(ghost, expectedCorners(entityVector(entity, "origin"), angles, bounds));
        }
    }
}

TEST_CASE("a model item places the model class setting with the model key and a class pick replaces it") {
    Document doc(parsed());
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(itemClasses());
    CHECK(session.modelPlacementClass() == "prop_static");
    session.setPlacementItem(ModelItem{"models/a.mdl"});
    REQUIRE(session.perform("tool.entity"));
    session.pointerClick(Ray{{21, 37, 100}, {0, 0, -1}});
    auto id = std::get<NodeId>(*session.selection().active());
    CHECK(doc.get(*doc.handle(id))->block->find("classname")->decodedValue() == "prop_static");
    CHECK(doc.get(*doc.handle(id))->block->find("model")->decodedValue() == "models/a.mdl");
    session.setModelPlacementClass("prop_dynamic");
    session.pointerClick(Ray{{121, 37, 100}, {0, 0, -1}});
    id = std::get<NodeId>(*session.selection().active());
    CHECK(doc.get(*doc.handle(id))->block->find("classname")->decodedValue() == "prop_dynamic");
    session.setPlacementItem(ClassItem{"prop_static"});
    session.pointerClick(Ray{{221, 37, 100}, {0, 0, -1}});
    id = std::get<NodeId>(*session.selection().active());
    CHECK_FALSE(doc.get(*doc.handle(id))->block->find("model"));
}

TEST_CASE("placement items are exclusive, class items have no ghost and Leave clears it") {
    Document doc(parsed());
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(itemClasses());
    session.setCurrentEntityClass("prop_dynamic");
    CHECK(std::get<ClassItem>(session.placementItem()).classname == "prop_dynamic");
    session.setPlacementItem(ModelItem{"models/a.mdl"});
    CHECK(std::get<ModelItem>(session.placementItem()).model == "models/a.mdl");
    session.setPlacementItem(PrefabItem{"p.vmf", {}, {}});
    CHECK(session.placementModel().empty());
    REQUIRE(session.placementPrefab());
    CHECK(std::holds_alternative<PrefabItem>(session.placementItem()));
    session.setPlacementItem(ModelItem{"models/b.mdl"});
    CHECK_FALSE(session.placementPrefab());
    session.setPlacementItem(PrefabItem{"p.vmf", {}, {}});
    session.setCurrentEntityClass("prop_dynamic"); // Choosing a class disarms the prefab.
    CHECK_FALSE(session.placementPrefab());
    CHECK(std::holds_alternative<ClassItem>(session.placementItem()));
    session.setPlacementItem(PrefabItem{"p.vmf", {}, {}});
    session.attach(doc); // Document replacement clears it.
    CHECK_FALSE(session.placementPrefab());

    const Ray ray{{21, 37, 100}, {0, 0, -1}};
    REQUIRE(session.perform("tool.entity"));
    session.pointerHover(ray);
    CHECK(session.preview().empty());
    session.setPlacementItem(ModelItem{"models/a.mdl"});
    session.pointerHover(ray);
    CHECK_FALSE(session.preview().empty());
    InputEvent leave;
    leave.kind = InputKind::Leave;
    session.input(leave, {});
    CHECK(session.preview().empty());
    session.pointerHover(ray);
    session.setModelPlacementClass("brush"); // Not a point class: nothing would land, so no ghost.
    session.pointerHover(ray);
    CHECK(session.preview().empty());
}

TEST_CASE("a prefab item's ghost equals the placement its placer receives and the placer owns the edit") {
    const AABB bounds{{-8, -8, 0}, {8, 24, 40}};
    for (const bool align : {false, true}) {
        INFO("align=" << align);
        Document doc(parsed());
        addBox(doc, {-16, -64, -64}, {0, 64, 64});
        EditorSession session;
        session.attach(doc);
        session.setAlignToSurface(align);
        std::optional<EntityPlacement> received;
        const auto history = doc.undoCount();
        session.setPlacementItem(PrefabItem{"prefabs/room.vmf", bounds,
            [&](EditorSession&, const EntityPlacement& placement) -> Result<void> {
                received = placement;
                return {};
            }});
        REQUIRE(session.perform("tool.entity"));
        CHECK(session.activeToolObject().readout().detail == "Prefab (room.vmf)");
        const Ray ray{{100, 5, 9}, {-1, 0, 0}};
        session.pointerHover(ray);
        REQUIRE_FALSE(session.preview().empty());
        const auto ghost = ghostCorners(session.preview());
        session.pointerClick(ray);
        REQUIRE(received);
        CHECK(received->angles.has_value() == align);
        checkCorners(ghost, expectedCorners(received->origin, received->angles, bounds));
        CHECK(doc.undoCount() == history);
        CHECK(std::holds_alternative<PrefabItem>(session.placementItem())); // Stays armed.
    }
}
