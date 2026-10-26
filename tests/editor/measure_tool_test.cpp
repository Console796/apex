#include "face_fixtures.hpp"
#include "editor/measure_tool.hpp"
using namespace face_test;

TEST_CASE("Measure helper distance delta and angle between face normals") {
    const auto m = measure({1, 2, 3}, {4, 6, 15}, DVec3{1, 0, 0}, DVec3{0, 0, 1});
    CHECK(m.distance == 13); CHECK(m.delta == DVec3{3, 4, 12});
    REQUIRE(m.faceAngle); CHECK(*m.faceAngle == 90);
    CHECK_FALSE(measure({0, 0, 0}, {1, 1, 1}).faceAngle);
    CHECK(*measure({0, 0, 0}, {0, 0, 0}, DVec3{0, 0, 2}, DVec3{0, 0, -1}).faceAngle == 180);
}
TEST_CASE("Measure tool uses shared Snapper overlays normals third click and Escape without document edits") {
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) {
        Scene s; s.session.setPreset(*preset); REQUIRE(s.session.perform("tool.measure"));
        const auto undo = s.doc.undoCount();
        const auto before = s.doc.write().value();
        auto e = click(); e.alt = preset -> name == "Hammer"; e.ctrl = preset -> name == "Blender";
        REQUIRE(s.session.input(e, view(Scene::ray({1, 0, 0}))));
        CHECK(s.session.activeToolObject().overlay({}, 1).worldPoints.size() == 1);
        REQUIRE(s.session.input(e, view(Scene::ray({0, 0, 1}))));
        auto overlay = s.session.activeToolObject().overlay({}, 1);
        REQUIRE(overlay.worldSegments.size() == 1); CHECK(overlay.worldPoints.size() == 2);
        REQUIRE(overlay.labels.size() == 1); CHECK(overlay.labels[0].text.find("face angle 90") != std::string::npos);
        CHECK(overlay.labels[0].text.find("\n") != std::string::npos);
        CHECK(overlay.labels[0].position == DVec3{32, 0, 32});
        CHECK(s.session.activeToolObject().readout().status.find("delta -64, 0, 64") != std::string::npos);
        REQUIRE(s.session.input(e, view(Scene::ray({0, 1, 0}))));
        overlay = s.session.activeToolObject().overlay({}, 1);
        CHECK(overlay.worldPoints.size() == 1); CHECK(overlay.worldSegments.empty());
        e.kind = InputKind::KeyDown; e.key = "Escape";
        REQUIRE(s.session.input(e, {})); CHECK(s.session.activeToolObject().overlay({}, 1).worldPoints.empty());
        CHECK(s.doc.undoCount() == undo); CHECK(s.doc.write().value() == before);
    }
}
TEST_CASE("Measure tool grid snaps via the common snapper") {
    Scene s; REQUIRE(s.session.perform("tool.measure")); s.session.setGridSize(16);
    const Ray ray{{256, 19, 21}, {-1, 0, 0}};
    const auto v = view(ray);
    const auto expected = s.session.snap(ray, v);
    REQUIRE(s.session.input(click(), v));
    const auto overlay = s.session.activeToolObject().overlay({}, 1);
    REQUIRE(overlay.worldPoints.size() == 1); CHECK(overlay.worldPoints[0].position == expected.point);
}
