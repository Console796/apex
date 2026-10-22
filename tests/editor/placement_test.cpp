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
