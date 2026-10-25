#include "face_fixtures.hpp"
using namespace face_test;

TEST_CASE("Apply tool Hammer applies wraps lifts and selects with Ctrl Shift") {
    Scene s;
    CHECK(s.session.selection().mode() == SelectionMode::Faces);
    const auto top = Scene::ray({0, 0, 1}), right = Scene::ray({1, 0, 0});
    const auto a = s.side({0, 0, 1}), b = s.side({1, 0, 0});
    REQUIRE(s.session.input(click(), view(top))); CHECK(s.session.selection().contains(a));
    REQUIRE(s.session.input(click(PointerButton::Left, false, true), view(right)));
    CHECK(s.session.selection().ordered().size() == 2); CHECK(s.session.selection().active() == Target{b});
    REQUIRE(s.session.input(click(PointerButton::Left, true), view(top)));
    CHECK_FALSE(s.session.selection().contains(a));
    REQUIRE(s.session.input(click(PointerButton::Left, true), view(top)));
    CHECK(s.session.selection().active() == Target{a});
    REQUIRE(s.session.rotateFaces(90));
    s.session.setNewSolidMaterial("apex/applied");
    const auto before = s.face({1, 0, 0});
    const auto undo = s.doc.undoCount();
    REQUIRE(s.session.input(click(PointerButton::Right), view(right)));
    CHECK(s.face({1, 0, 0}).face.material == "apex/applied");
    CHECK(changed(before, s.face({1, 0, 0})) == std::set<std::string>{"material"});
    CHECK(s.doc.undoCount() == undo + 1);
    s.session.setNewSolidMaterial("apex/wrapped");
    REQUIRE(s.session.input(click(PointerButton::Right, false, false, true), view(right)));
    CHECK(s.face({1, 0, 0}).face.material == "apex/wrapped");
    edgeEqual(s.face({0, 0, 1}), s.face({1, 0, 0}));
    CHECK(s.doc.undoCount() == undo + 2);
    REQUIRE(s.session.input(click(PointerButton::Left, false, false, true), view(right)));
    CHECK(s.session.newSolidStyle().material == "apex/wrapped"); CHECK(s.doc.undoCount() == undo + 2);
    CHECK(s.session.activeToolObject().readout().detail == "apex/wrapped");
}
TEST_CASE("Apply tool Blender Ctrl click applies Ctrl Alt wraps and right click stays context menu") {
    Scene s; s.session.setPreset(blenderPreset());
    const auto top = Scene::ray({0, 0, 1}), right = Scene::ray({1, 0, 0});
    s.session.input(click(), view(top));
    s.session.setNewSolidMaterial("apex/blender");
    const auto undo = s.doc.undoCount();
    CHECK_FALSE(s.session.input(click(PointerButton::Right), view(right)));
    CHECK(s.doc.undoCount() == undo);
    REQUIRE(s.session.input(click(PointerButton::Left, true), view(right)));
    CHECK(s.face({1, 0, 0}).face.material == "apex/blender");
    CHECK(s.session.selection().ordered().size() == 1);
    REQUIRE(s.session.input(click(PointerButton::Left, true, false, true), view(right)));
    edgeEqual(s.face({0, 0, 1}), s.face({1, 0, 0}));
    REQUIRE(s.session.input(click(PointerButton::Left, false, false, true), view(right)));
    CHECK(s.session.newSolidStyle().material == "apex/blender");
}
TEST_CASE("Apply wrapped without a source applies plainly and wraps faces that don't touch") {
    Scene s;
    s.session.setNewSolidMaterial("apex/plain");
    const auto before = s.face({1, 0, 0});
    const auto undo = s.doc.undoCount();
    const auto plain = s.session.applyMaterialWrapped(Scene::ray({1, 0, 0}));
    REQUIRE(plain); CHECK(*plain);
    CHECK(changed(before, s.face({1, 0, 0})) == std::set<std::string>{"material"});
    CHECK(s.doc.undoCount() == undo + 1);

    // Parallel faces copy the alignment.
    s.session.selection().select(s.side({-1, 0, 0}));
    REQUIRE(s.session.rotateFaces(30));
    REQUIRE(s.session.applyMaterialWrapped(Scene::ray({1, 0, 0})));
    const auto source = s.face({-1, 0, 0}).face.uv, copied = s.face({1, 0, 0}).face.uv;
    CHECK(glm::length(copied.uaxis - source.uaxis) < 1e-9);
    CHECK(copied.shiftU == doctest::Approx(source.shiftU));

    // A separate box: its +X wall continues the top face's texture along the line the planes share.
    auto tx = s.doc.begin("Separate authored box");
    tx.create(geometry::solidBlock(geometry_test::build(
        geometry_test::boxSides({200, -64, -64}, {328, 64, 64})), {"apex/separate"}));
    tx.commit();
    s.session.selection().select(s.side({0, 0, 1}));
    REQUIRE(s.session.rotateFaces(20));
    const Ray wall{{512, 0, 0}, {-1, 0, 0}};
    const auto wrapped = s.session.applyMaterialWrapped(wall);
    REQUIRE(wrapped); CHECK(*wrapped);
    const auto top = s.face({0, 0, 1});
    const auto target = *faceSnapshot(s.doc, *s.session.picks().first(wall) -> side);
    // Written axes are rounded to the file's precision, and whole textures apart is the same texel:
    // allow a hundredth of a texel.
    const auto texelGap = [](const FaceSnapshot& a, const FaceSnapshot& b, DVec3 p) {
        auto d = geometry::uvAt(a.face, p, 128, 64) - geometry::uvAt(b.face, p, 128, 64);
        return glm::length((d - glm::round(d)) * DVec2{128, 64});
    };
    for (double y : {-50.0, 0.0, 37.0}) CHECK(texelGap(top, target, {328, y, 64}) < 0.01);

    // Far from the origin with a small scale the unreduced shift is over a million texels, which six
    // significant digits would round by several texels.
    auto far = s.doc.begin("Far authored box");
    far.create(geometry::solidBlock(geometry_test::build(
        geometry_test::boxSides({16256, -64, -64}, {16384, 64, 64})), {"apex/far"}));
    far.commit();
    REQUIRE(s.session.setFaceField(FaceField::ScaleU, 0.01));
    REQUIRE(s.session.setFaceField(FaceField::Rotation, 37.0));
    const Ray farWall{{16640, 0, 0}, {-1, 0, 0}};
    REQUIRE(s.session.applyMaterialWrapped(farWall));
    const auto farTarget = *faceSnapshot(s.doc, *s.session.picks().first(farWall)->side);
    CHECK(std::abs(farTarget.face.uv.shiftU) < 256);
    for (double y : {-50.0, 0.0, 37.0}) CHECK(texelGap(s.face({0, 0, 1}), farTarget, {16384, y, 64}) < 0.01);
}

TEST_CASE("Apply wrapped accepts partially overlapping coplanar edges") {
    Scene s;
    auto tx = s.doc.begin("Adjacent authored box");
    tx.create(geometry::solidBlock(geometry_test::build(
        geometry_test::boxSides({-32, 64, -64}, {32, 128, 64})), {"apex/adjacent"}));
    tx.commit();
    s.session.selection().select(s.side({0, 0, 1}));
    s.session.setNewSolidMaterial("apex/partial");
    const Ray ray{{0, 96, 256}, {0, 0, -1}};
    const auto result = s.session.applyMaterialWrapped(ray);
    REQUIRE(result); CHECK(*result);
    const auto hit = s.session.picks().first(ray);
    REQUIRE(hit); REQUIRE(hit -> side);
    CHECK(faceSnapshot(s.doc, *hit -> side) -> face.material == "apex/partial");
}

TEST_CASE("Face extrapolation Projected Match and aligned lift update only the intended face") {
    Scene s;
    const auto top = s.side({0, 0, 1}), right = s.side({1, 0, 0});
    s.session.selection().select(top);
    REQUIRE(s.session.rotateFaces(45));
    const auto source = s.face({0, 0, 1}).face.uv;
    s.session.setFaceExtrapolation(FaceExtrapolation::Projected);
    s.session.setNewSolidMaterial("apex/projected");
    // Onto a perpendicular wall both floor axes collapse onto the shared edge; the parallel bottom keeps them.
    const auto projected = s.session.applyMaterialWrapped(Scene::ray({0, 0, -1}));
    REQUIRE(projected); CHECK(projected->applied); CHECK_FALSE(projected->fellBackToWrap);
    const auto projectedFace = s.face({0, 0, -1});
    CHECK(glm::length(projectedFace.face.uv.uaxis - source.uaxis) < 1e-9);
    CHECK(glm::length(projectedFace.face.uv.vaxis - source.vaxis) < 1e-9);

    s.session.setFaceExtrapolation(FaceExtrapolation::Match);
    s.session.setNewSolidMaterial("apex/matched");
    REQUIRE(s.session.applyMaterialWrapped(Scene::ray({1, 0, 0})));
    const auto matched = s.face({1, 0, 0}).face.uv;
    CHECK(geometry::deriveRotation(matched, {1, 0, 0}) == doctest::Approx(geometry::deriveRotation(source, {0, 0, 1})));
    CHECK(matched.shiftU == source.shiftU); CHECK(matched.shiftV == source.shiftV);
    CHECK(matched.scaleU == source.scaleU); CHECK(matched.scaleV == source.scaleV);

    // A rotated floor projects degenerately onto the perpendicular +X target and falls back.
    s.session.selection().select(top);
    s.session.setFaceExtrapolation(FaceExtrapolation::Projected);
    s.session.setNewSolidMaterial("apex/fallback");
    const auto fallback = s.session.applyMaterialWrapped(Scene::ray({1, 0, 0}));
    REQUIRE(fallback); CHECK(fallback->applied); CHECK(fallback->fellBackToWrap);
    CHECK(s.face({1, 0, 0}).face.material == "apex/fallback");
    REQUIRE(s.session.input(click(PointerButton::Right, false, false, true), view(Scene::ray({1, 0, 0}))));
    CHECK(s.session.activeToolObject().readout().status.find("Projected was degenerate") != std::string::npos);

    REQUIRE(s.session.input(click(PointerButton::Left, false, true, true), view(Scene::ray({1, 0, 0}))));
    CHECK(s.session.selection().ordered().size() == 1);
    CHECK(s.session.selection().active() == Target{right});
    CHECK(s.session.newSolidStyle().material == "apex/fallback");
}

TEST_CASE("Blender lift with alignment uses Ctrl Shift Alt and makes the clicked face the source") {
    Scene s; s.session.setPreset(blenderPreset());
    const auto top = s.side({0, 0, 1});
    REQUIRE(s.session.applyMaterialAt(Scene::ray({0, 0, 1}), "apex/lifted"));
    REQUIRE(s.session.input(click(PointerButton::Left, true, true, true), view(Scene::ray({0, 0, 1}))));
    CHECK(s.session.newSolidStyle().material == "apex/lifted");
    CHECK(s.session.selection().ordered().size() == 1);
    CHECK(s.session.selection().active() == Target{top});
    s.session.setNewSolidMaterial("apex/after-lift");
    REQUIRE(s.session.applyMaterialWrapped(Scene::ray({1, 0, 0})));
    CHECK(s.face({1, 0, 0}).face.material == "apex/after-lift");
}
