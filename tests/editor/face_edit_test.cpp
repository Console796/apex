#include "face_fixtures.hpp"
using namespace face_test;

TEST_CASE("Face fields write only affected keys and derive rotation only when axes change") {
    for (const auto field : {FaceField::Material, FaceField::ShiftU, FaceField::ShiftV, FaceField::ScaleU, FaceField::ScaleV,
                            FaceField::Rotation, FaceField::LightmapScale, FaceField::SmoothingGroups}) {
        CAPTURE(static_cast<int>(field));
        Scene s;
        const auto side = s.side({0, 0, 1});
        s.session.selection().select(side);
        auto tx = s.doc.begin("Contradictory rotation"); tx.setSideKeyValue(side, "rotation", "123"); tx.commit();
        const auto before = s.face({0, 0, 1}), other = s.face({1, 0, 0});
        const auto undo = s.doc.undoCount();
        const FaceValue value = field == FaceField::Material ? FaceValue{std::string{"apex/new"}} : FaceValue{32.};
        const auto result = s.session.setFaceField(field, value);
        REQUIRE(result); CHECK(*result == 1);
        const auto after = s.face({0, 0, 1});
        std::set<std::string> expected;
        switch(field) {
        case FaceField::Material: expected = {"material"}; break;
        case FaceField::ShiftU: case FaceField::ScaleU: expected = {"uaxis", "rotation"}; break;
        case FaceField::ShiftV: case FaceField::ScaleV: expected = {"vaxis", "rotation"}; break;
        case FaceField::Rotation: expected = {"uaxis", "vaxis", "rotation"}; break;
        case FaceField::LightmapScale: expected = {"lightmapscale"}; break;
        case FaceField::SmoothingGroups: expected = {"smoothing_groups"}; break;
        }
        CHECK(changed(before, after) == expected);
        CHECK(keys(other) == keys(s.face({1, 0, 0})));
        if (expected.contains("rotation")) CHECK(after.keys.find("rotation") -> asDouble().value() == doctest::Approx(geometry::deriveRotation(after.face.uv, after.face.plane.normal)));
        CHECK(s.doc.undoCount() == undo + 1);
        REQUIRE(s.doc.undo()); CHECK(keys(s.face({0, 0, 1})) == keys(before));
        REQUIRE(s.doc.redo());
        const auto count = s.doc.undoCount();
        REQUIRE(s.session.setFaceField(field, value)); CHECK(s.doc.undoCount() == count);
    }
}
TEST_CASE("Face summary mixed fields survive other edits and typing coalesces per field and selection") {
    Scene s;
    const auto a = s.side({0, 0, 1}), b = s.side({0, 0, -1});
    s.session.selection().select(a);
    REQUIRE(s.session.setFaceField(FaceField::ShiftU, 10.));
    REQUIRE(s.session.setFaceField(FaceField::Material, std::string{"apex/other"}));
    s.session.selection().add(b);
    const std::array ids{a, b};
    const auto props = summarize(s.doc, ids);
    CHECK(props.count == 2); CHECK_FALSE(props.material); CHECK_FALSE(props.shiftU);
    REQUIRE(props.scaleU); CHECK(*props.scaleU == .25);
    const auto undo = s.doc.undoCount();
    REQUIRE(s.session.setFaceField(FaceField::LightmapScale, 32, "focus"));
    REQUIRE(s.session.setFaceField(FaceField::LightmapScale, 64, "focus"));
    CHECK(s.doc.undoCount() == undo + 1);
    CHECK(s.face({0, 0, 1}).face.uv.shiftU == 10);
    CHECK(s.face({0, 0, -1}).face.uv.shiftU == 0);
    CHECK_FALSE(summarize(s.doc, ids).material);
    REQUIRE(s.doc.undo()); CHECK(s.face({0, 0, 1}).face.lightmapScale == 16);
    CHECK(summarize(s.doc, {}).count == 0);
    s.session.selection().setMode(SelectionMode::Solids);
    const auto count = s.doc.undoCount();
    REQUIRE(s.session.setFaceField(FaceField::ShiftU, 20.)); CHECK(s.doc.undoCount() == count);
}
TEST_CASE("Face justify treat as one uses combined bounds and alignment preserves fields") {
    Scene s;
    const auto a = s.side({0, 0, 1}), b = s.side({0, 0, -1});
    // Two coplanar authored boxes, separated in X, with the same default texture axes.
    auto tx = s.doc.begin("Second authored UV box");
    tx.create(geometry::solidBlock(geometry_test::build(geometry_test::boxSides({128, -64, -64}, {256, 64, 64})), {"apex/base"})); tx.commit();
    const auto second=*s.session.picks().first({{192, 0, 256}, {0, 0, -1}}) -> side;
    s.session.selection().select(a); s.session.selection().add(second);
    REQUIRE(s.session.justifyFaces(geometry::Justify::Fit, true));
    CHECK(faceSnapshot(s.doc, a) -> face.uv.scaleU == doctest::Approx(2.5));
    CHECK(faceSnapshot(s.doc, second) -> face.uv.scaleU == doctest::Approx(2.5));
    CHECK(faceSnapshot(s.doc, a) -> face.uv.shiftU == doctest::Approx(25.6));
    CHECK(faceSnapshot(s.doc, second) -> face.uv.shiftU == doctest::Approx(25.6));
    REQUIRE(s.doc.undo());
    REQUIRE(s.session.justifyFaces(geometry::Justify::Fit, false));
    CHECK(faceSnapshot(s.doc, a) -> face.uv.scaleU == doctest::Approx(1));
    CHECK(faceSnapshot(s.doc, second) -> face.uv.scaleU == doctest::Approx(1));
    s.session.selection().select(b);
    REQUIRE(s.session.rotateFaces(90));
    REQUIRE(s.session.alignFaces(FaceAlignment::World));
    CHECK(geometry::deriveRotation(s.face({0, 0, -1}).face.uv, {0, 0, -1}) == 0);
}
TEST_CASE("Face field invalid input is atomic and leaves no undo entry") {
    Scene s; s.session.selection().select(s.side({0, 0, 1}));
    const auto undo = s.doc.undoCount();
    CHECK_FALSE(s.session.setFaceField(FaceField::ScaleU, 0.));
    CHECK_FALSE(s.session.setFaceField(FaceField::ShiftV, INFINITY));
    CHECK_FALSE(s.session.setFaceField(FaceField::LightmapScale, -1));
    CHECK_FALSE(s.session.setFaceField(FaceField::SmoothingGroups, -1));
    CHECK(s.doc.undoCount() == undo);
}

TEST_CASE("Face edits without a typing token each create one undo step") {
    Scene s;
    s.session.selection().select(s.side({0, 0, 1}));
    const auto undo = s.doc.undoCount();
    REQUIRE(s.session.setFaceField(FaceField::LightmapScale, 32));
    REQUIRE(s.session.setFaceField(FaceField::LightmapScale, 64));
    CHECK(s.doc.undoCount() == undo + 2);
    REQUIRE(s.doc.undo());
    CHECK(s.face({0, 0, 1}).face.lightmapScale == 32);
    const auto count = s.doc.undoCount();
    REQUIRE(s.session.applyMaterialAt(Scene::ray({0, 0, 1}), "apex/first"));
    REQUIRE(s.session.applyMaterialAt(Scene::ray({0, 0, 1}), "apex/second"));
    CHECK(s.doc.undoCount() == count + 2);
}

TEST_CASE("Face summary shows rotation mixed for a U axis along the normal instead of throwing") {
    Scene s;
    const auto top = s.side({0, 0, 1});
    auto tx = s.doc.begin("U axis along the normal");
    tx.setSideKeyValue(top, "uaxis", "[0 0 1 0] 0.25");
    tx.commit();
    const std::array ids{top};
    const auto props = summarize(s.doc, ids);
    CHECK(props.count == 1);
    CHECK_FALSE(props.rotation);
    REQUIRE(props.shiftU); CHECK(*props.shiftU == 0);
}
TEST_CASE("UV gestures preserve pivots, apply to all faces and coalesce as one edit") {
    Scene s;
    const auto top = s.side({0, 0, 1}), bottom = s.side({0, 0, -1});
    s.session.selection().select(top);
    s.session.selection().add(bottom);
    const auto first = faceSnapshot(s.doc, top).value();
    const auto second = faceSnapshot(s.doc, bottom).value();
    DVec3 activePivot{};
    for (const auto p : second.points) activePivot += p;
    activePivot /= static_cast<double>(second.points.size());
    const auto texel = [&](const FaceSnapshot& face, DVec3 point) {
        return geometry::uvAt(face.face, point, 128, 64);
    };
    const auto beforeActive = texel(second, activePivot);
    DVec3 otherPivot{};
    for (const auto p : first.points) otherPivot += p;
    otherPivot /= static_cast<double>(first.points.size());
    const auto beforeOther = texel(first, otherPivot);
    const auto undo = s.doc.undoCount();
    REQUIRE(s.session.editFaceGesture(FaceGesture::Rotate, 30, 0, activePivot, "drag"));
    REQUIRE(s.session.editFaceGesture(FaceGesture::Rotate, 10, 0, activePivot, "drag"));
    CHECK(s.doc.undoCount() == undo + 1);
    CHECK(texel(faceSnapshot(s.doc, bottom).value(), activePivot) == beforeActive);
    CHECK(texel(faceSnapshot(s.doc, top).value(), otherPivot) == beforeOther);
    for (const auto side : {top, bottom}) {
        const auto after = faceSnapshot(s.doc, side).value();
        CHECK(after.keys.find("material")->decodedValue() == "apex/base");
        CHECK(after.keys.find("rotation")->asDouble().value() == doctest::Approx(
            geometry::deriveRotation(after.face.uv, after.face.plane.normal)));
    }
    REQUIRE(s.doc.undo());
    CHECK(faceSnapshot(s.doc, top)->face.uv.uaxis == first.face.uv.uaxis);
    CHECK(faceSnapshot(s.doc, bottom)->face.uv.uaxis == second.face.uv.uaxis);
}
