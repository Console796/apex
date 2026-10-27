#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/placement.hpp"

using namespace editor_test;

TEST_CASE("placement support is flush for floor wall ceiling and slope") {
    const AABB box{{-3, -5, -7}, {11, 13, 17}};
    const DVec3 hit{19.3, -44.7, 8.1};
    for (const auto normal : {DVec3{0, 0, 1}, DVec3{1, 0, 0}, DVec3{0, -1, 0},
                              DVec3{0, 0, -1}, DVec3{1, 2, 3}, DVec3{-2, 4, -1}}) {
        for (const bool align : {false, true}) {
            INFO(normal.x, ",", normal.y, ",", normal.z, " align=", align);
            const auto placed = placeOnSurface(hit, normal, box, align);
            REQUIRE(placed);
            const auto unit = glm::normalize(normal);
            const auto basis = placed->angles ? basisFromAngles(*placed->angles) : DMat3{1};
            if (align) checkPoint(angleVectors(*placed->angles).up, unit);
            double minimum = 1e99;
            for (unsigned c = 0; c < 8; ++c) {
                const DVec3 corner{c & 1 ? box.max.x : box.min.x, c & 2 ? box.max.y : box.min.y,
                                   c & 4 ? box.max.z : box.min.z};
                const double distance = glm::dot(unit, placed->origin + basis * corner - hit);
                CHECK(distance >= -1e-6);
                minimum = std::min(minimum, distance);
            }
            CHECK(std::abs(minimum) < 1e-6);
        }
    }
    CHECK_FALSE(placeOnSurface(hit, {0, 0, 0}, box));
    CHECK_FALSE(placeOnSurface(hit, {0, 0, 1}, AABB{}));
}

TEST_CASE("session places point entities in one undo step and restores selection") {
    Document doc(parsed());
    EditorSession session;
    session.attach(doc);
    assets::EntityClassTable table;
    assets::EntityClass cls;
    cls.name = "test_point";
    cls.type = "PointClass";
    cls.mins = {-3, -5, -7};
    cls.maxs = {11, 13, 17};
    table.classes[cls.name] = cls;
    cls.type = "SolidClass";
    table.classes["brush"] = cls;
    session.setEntityClasses(std::move(table));
    REQUIRE(session.placeEntity("test_point", DVec3{19.3, -44.7, 8.1}, DVec3{0, 0, 1}));
    CHECK(doc.undoLabels() == std::vector<std::string>{"Place entity"});
    REQUIRE(session.selection().ordered().size() == 1);
    const auto entity = std::get<NodeId>(session.selection().ordered().front());
    auto origin = doc.get(*doc.handle(entity))->block->find("origin")->asVec3().value();
    checkPoint({origin[0], origin[1], origin[2]}, {19.3, -44.7, 15.1});
    CHECK(session.picks().bounds(entity)->min.z == doctest::Approx(8.1));
    REQUIRE(session.perform("edit.undo"));
    CHECK_FALSE(doc.handle(entity));
    CHECK(session.selection().ordered().empty());
    REQUIRE(session.perform("edit.redo"));
    CHECK(session.selection().contains(Target{entity}));
    CHECK_FALSE(session.placeEntity("brush", DVec3{0}, DVec3{0, 0, 1}));
    CHECK_FALSE(session.placeEntity("absent", DVec3{0}, DVec3{0, 0, 1}));
    session.setGridSize(16);
    REQUIRE(session.placeEntity("test_point", Ray{{21, 37, 100}, {0, 0, -1}}));
    const auto snapped = std::get<NodeId>(*session.selection().active());
    origin = doc.get(*doc.handle(snapped))->block->find("origin")->asVec3().value();
    checkPoint({origin[0], origin[1], origin[2]}, {16, 32, 7});
    CHECK(doc.undoCount() == 2);
    addBox(doc, {-100, -100, 10}, {100, 100, 20});
    session.pinAxisWorkPlane(Axis::X, {500, 0, 0});
    const auto history = doc.undoCount();
    REQUIRE(session.placeEntity("test_point", Ray{{21, 37, 100}, {0, 0, -1}}));
    const auto onSurface = std::get<NodeId>(*session.selection().active());
    origin = doc.get(*doc.handle(onSurface))->block->find("origin")->asVec3().value();
    checkPoint({origin[0], origin[1], origin[2]}, {21, 37, 27});
    CHECK(doc.undoCount() == history + 1);
    REQUIRE(session.placeEntity("test_point", Ray{{1000, 37, 100}, {-1, 0, 0}}));
    const auto onPlane = std::get<NodeId>(*session.selection().active());
    origin = doc.get(*doc.handle(onPlane))->block->find("origin")->asVec3().value();
    checkPoint({origin[0], origin[1], origin[2]}, {503, 32, 96});
}

namespace {
assets::EntityClassTable modelPlacementClasses() {
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
}

TEST_CASE("session model placement uses model bounds or the FGD fallback in one undo step") {
    const DVec3 hit{19.3, -44.7, 8.1}, normal{1, 0, 0};
    const std::string path = "models/props/barrel.mdl";
    for (const int provider : {0, 1, 2, 3}) {
        for (const bool align : {false, true}) {
            INFO("provider=" << provider << " align=" << align);
            Document doc(parsed());
            EditorSession session;
            session.attach(doc);
            session.setEntityClasses(modelPlacementClasses());
            const AABB bounds = provider >= 2 ? AABB{{-10, -20, -30}, {10, 20, 30}}
                                              : AABB{{-3, -5, -7}, {11, 13, 17}};
            bool asked = false;
            if (provider) session.setEntityBoundsProvider([&](const kv1::Node& entity) -> std::optional<EntityPickShape> {
                asked = true;
                CHECK(entity.find("classname")->decodedValue() == "prop_static");
                CHECK(entity.find("model")->decodedValue() == path);
                if (provider == 1) return {};
                if (provider == 2) return EntityPickShape{{}, 0, {bounds.min, bounds.max}};
                return EntityPickShape{std::make_shared<PickModel>(
                    std::vector<DVec3>{bounds.min, {bounds.max.x, bounds.min.y, bounds.max.z}, bounds.max},
                    std::vector<std::uint32_t>{0, 1, 2}), 0, {}};
            });
            const auto before = doc.write().value();
            REQUIRE(session.placeModel(path, hit, normal, align));
            CHECK(asked == (provider != 0));
            CHECK(doc.undoLabels() == std::vector<std::string>{"Place model"});
            REQUIRE(session.selection().ordered().size() == 1);
            const auto id = std::get<NodeId>(*session.selection().active());
            const auto& entity = *doc.get(*doc.handle(id))->block;
            CHECK(entity.find("classname")->decodedValue() == "prop_static");
            CHECK(entity.find("model")->decodedValue() == path);
            const auto expected = placeOnSurface(hit, normal, bounds, align);
            REQUIRE(expected);
            checkPoint(entityVector(entity, "origin"), expected->origin);
            if (align) {
                checkPoint(entityVector(entity, "angles"), *expected->angles);
                checkPoint(angleVectors(entityVector(entity, "angles")).up, normal);
            } else CHECK_FALSE(entity.find("angles"));
            REQUIRE(session.perform("edit.undo"));
            CHECK_FALSE(doc.handle(id));
            CHECK(doc.write().value() == before);
            CHECK(session.selection().ordered().empty());
        }
    }
}

TEST_CASE("session model placement rejects invalid classes and class picks clear the model") {
    Document doc(parsed());
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(modelPlacementClasses());
    CHECK(session.modelPlacementClass() == "prop_static");
    session.setCurrentEntityClass("prop_static");
    session.setPlacementModel("models/props/barrel.mdl");
    CHECK(session.placementModel() == "models/props/barrel.mdl");
    session.setCurrentEntityClass("prop_static"); // Even choosing the same class disarms the model.
    CHECK(session.placementModel().empty());
    session.setPlacementModel("models/props/barrel.mdl");
    session.setPlacementModel({});
    CHECK(session.placementModel().empty());
    const auto before = doc.write().value();
    for (const auto* classname : {"absent", "brush"}) {
        session.setModelPlacementClass(classname);
        const auto result = session.placeModel("models/props/barrel.mdl", DVec3{0}, DVec3{1, 0, 0}, true);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::InvalidArgument);
        CHECK(result.error().what() == "choose a point entity class");
        CHECK(doc.write().value() == before);
        CHECK(doc.undoCount() == 0);
        CHECK(session.selection().ordered().empty());
    }
    session.setModelPlacementClass("prop_dynamic");
    REQUIRE(session.placeModel("models/props/barrel.mdl", DVec3{0}, DVec3{0, 0, 1}));
    const auto id = std::get<NodeId>(*session.selection().active());
    CHECK(doc.get(*doc.handle(id))->block->find("classname")->decodedValue() == "prop_dynamic");
    session.setPlacementModel("models/props/barrel.mdl");
    Document replacement(parsed());
    session.attach(replacement);
    CHECK(session.placementModel().empty());
    session.detach();
}

TEST_CASE("Entity tool places its armed model on a wall and keeps it armed") {
    Document doc(parsed());
    addBox(doc, {-16, -64, -64}, {0, 64, 64});
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(modelPlacementClasses());
    session.setCurrentEntityClass("prop_dynamic");
    session.setPlacementModel("models/props/barrel.mdl");
    session.setAlignToSurface(true);
    REQUIRE(session.perform("tool.entity"));
    CHECK(session.activeToolObject().readout().detail == "prop_static (barrel.mdl)");
    CHECK(session.activeToolObject().readout().status == "Entity: prop_static (barrel.mdl)");
    const auto history = doc.undoCount();
    session.pointerClick(Ray{{100, 0, 0}, {-1, 0, 0}});
    CHECK(doc.undoCount() == history + 1);
    const auto id = std::get<NodeId>(*session.selection().active());
    const auto& entity = *doc.get(*doc.handle(id))->block;
    CHECK(entity.find("classname")->decodedValue() == "prop_static");
    CHECK(entity.find("model")->decodedValue() == "models/props/barrel.mdl");
    checkPoint(angleVectors(entityVector(entity, "angles")).up, {1, 0, 0});
    checkPoint(entityVector(entity, "origin"), {7, 0, 0});
    CHECK(session.placementModel() == "models/props/barrel.mdl");
    session.setCurrentEntityClass("prop_dynamic");
    session.pointerClick(Ray{{100, 30, 0}, {-1, 0, 0}});
    const auto plain = std::get<NodeId>(*session.selection().active());
    CHECK(doc.get(*doc.handle(plain))->block->find("classname")->decodedValue() == "prop_dynamic");
    CHECK_FALSE(doc.get(*doc.handle(plain))->block->find("model"));
}

TEST_CASE("session model ray placement skips point boxes and uses the first model surface") {
    Document doc(parsed());
    addBlock(doc, "entity { classname prop_static model models/wall.mdl }");
    addBlock(doc, "entity { classname prop_dynamic origin \"50 0 0\" }");
    addBox(doc, {-40, -64, -64}, {-30, 64, 64});
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(modelPlacementClasses());
    const auto wall = std::make_shared<PickModel>(
        std::vector<DVec3>{{0, -64, -64}, {0, 64, -64}, {0, 0, 64}},
        std::vector<std::uint32_t>{0, 1, 2});
    session.setEntityBoundsProvider([wall](const kv1::Node& entity) -> std::optional<EntityPickShape> {
        const auto* model = entity.find("model");
        if (model && model->decodedValue() == "models/wall.mdl") return EntityPickShape{wall, 0, {}};
        return {};
    });
    session.pinAxisWorkPlane(Axis::X, {500, 0, 0});
    REQUIRE(session.placeModel("models/placed.mdl", Ray{{100, 0, 0}, {-1, 0, 0}}, true));
    const auto id = std::get<NodeId>(*session.selection().active());
    const auto& entity = *doc.get(*doc.handle(id))->block;
    checkPoint(entityVector(entity, "origin"), {7, 0, 0});
    checkPoint(angleVectors(entityVector(entity, "angles")).up, {1, 0, 0});
}

TEST_CASE("session model placement falls back to the snapped work plane and default class box") {
    Document doc(parsed());
    EditorSession session;
    const auto detached = session.placeModel("models/placed.mdl", Ray{{21, 37, 100}, {0, 0, -1}});
    REQUIRE_FALSE(detached);
    CHECK(detached.error().what() == "no document");
    session.attach(doc);
    auto table = modelPlacementClasses();
    table.classes["prop_static"].mins.reset();
    table.classes["prop_static"].maxs.reset();
    session.setEntityClasses(std::move(table));
    session.setGridSize(16);
    REQUIRE(session.placeModel("models/placed.mdl", Ray{{21, 37, 100}, {0, 0, -1}}));
    const auto id = std::get<NodeId>(*session.selection().active());
    const auto& entity = *doc.get(*doc.handle(id))->block;
    checkPoint(entityVector(entity, "origin"), {16, 32, 4});
    CHECK_FALSE(entity.find("angles"));
    const auto before = doc.write().value();
    for (const auto ray : {Ray{{100, 0, 100}, {1, 0, 0}}, Ray{{100, 0, 100}, {0, 0, 1}}}) {
        const auto result = session.placeModel("models/placed.mdl", ray);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::InvalidArgument);
        CHECK(doc.write().value() == before);
        CHECK(doc.undoCount() == 1);
    }
}
