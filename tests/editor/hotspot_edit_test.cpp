// Plan 0.8 D13 (0.8.7): hotspotting selected faces and the apply tool's Hotspot mode, as edits:
// one undo step per click, a new seed per click, redo replaying the one drawn.
#include "face_fixtures.hpp"
#include "editor/apply_tool.hpp"

#include <set>

using namespace face_test;

namespace {

using Rects = std::vector<geometry::HotspotRect>;

// The texel bounds of a face under its own mapping.
std::array<double, 4> texelBounds(const FaceSnapshot& f) {
    std::array<double, 4> b{1e18, -1e18, 1e18, -1e18};
    for (const auto& p : f.points) {
        const double u = glm::dot(p, f.face.uv.uaxis) / f.face.uv.scaleU + f.face.uv.shiftU;
        const double v = glm::dot(p, f.face.uv.vaxis) / f.face.uv.scaleV + f.face.uv.shiftV;
        b = {std::min(b[0], u), std::max(b[1], u), std::min(b[2], v), std::max(b[3], v)};
    }
    return b;
}
bool fills(const FaceSnapshot& f, const geometry::HotspotRect& r) {
    const auto b = texelBounds(f);
    return std::abs(b[0] - r.x1) < 1e-6 && std::abs(b[1] - r.x2) < 1e-6 && std::abs(b[2] - r.y1) < 1e-6 &&
           std::abs(b[3] - r.y2) < 1e-6;
}
void hotspots(Scene& s, std::string material, Rects rects) {
    s.session.faceHotspots = [material, rects](std::string_view name) { return name == material ? rects : Rects{}; };
}

} // namespace

TEST_CASE("Hotspot faces fits each selected face as one undo step, and redo replays it") {
    Scene s;
    // The box's 128-unit faces are 512 texels at the default 0.25: rect 0 exactly.
    const Rects rects{{0, 0, 512, 512}, {0, 512, 512, 640}};
    hotspots(s, "apex/base", rects);
    const auto top = s.side({0, 0, 1}), side = s.side({1, 0, 0});
    s.session.selection().select(top);
    s.session.selection().add(side);
    const auto undo = s.doc.undoCount();
    const auto result = s.session.hotspotFaces();
    REQUIRE(result);
    CHECK(*result == 2);
    CHECK(s.doc.undoCount() == undo + 1);
    CHECK(s.doc.undoLabels().back() == "Hotspot faces");
    CHECK(fills(s.face({0, 0, 1}), rects[0]));
    CHECK(fills(s.face({1, 0, 0}), rects[0]));
    const auto fitted = keys(s.face({0, 0, 1}));
    REQUIRE(s.doc.undo());
    CHECK_FALSE(fills(s.face({0, 0, 1}), rects[0]));
    REQUIRE(s.doc.redo());
    CHECK(keys(s.face({0, 0, 1})) == fitted);
}

TEST_CASE("Hotspot faces draws a new seed per click among equal rects") {
    Scene s;
    const Rects rects{{0, 0, 512, 512}, {512, 0, 1024, 512}, {0, 512, 512, 1024}, {512, 512, 1024, 1024}};
    hotspots(s, "apex/base", rects);
    s.session.selection().select(s.side({0, 0, 1}));
    std::set<std::string> mappings;
    for (int click = 0; click < 16; ++click) {
        REQUIRE(s.session.hotspotFaces());
        const auto face = s.face({0, 0, 1});
        CHECK(std::ranges::any_of(rects, [&](const auto& r) { return fills(face, r); }));
        mappings.insert(keys(face)["uaxis"] + keys(face)["vaxis"]);
    }
    CHECK(mappings.size() > 1);
}

TEST_CASE("Hotspot faces skips faces without rects, uses the alternate group on request, and refuses with none") {
    Scene s;
    const Rects rects{{0, 0, 512, 512}, {0, 512, 512, 1024, false, false, true}};
    hotspots(s, "apex/trim", rects);
    const auto top = s.side({0, 0, 1}), bottom = s.side({0, 0, -1});
    s.session.selection().select(top);
    const auto undo = s.doc.undoCount();
    const auto none = s.session.hotspotFaces();
    REQUIRE_FALSE(none);
    CHECK(std::string(none.error().what()).find("apex/base has no hotspot rects") != std::string::npos);
    CHECK(s.doc.undoCount() == undo);

    REQUIRE(s.session.setFaceField(FaceField::Material, std::string{"apex/trim"}));
    s.session.selection().add(bottom); // Still apex/base: skipped.
    const auto before = keys(s.face({0, 0, -1}));
    auto result = s.session.hotspotFaces(true);
    REQUIRE(result);
    CHECK(*result == 1);
    CHECK(fills(s.face({0, 0, 1}), rects[1]));
    CHECK(keys(s.face({0, 0, -1})) == before);
    result = s.session.hotspotFaces();
    REQUIRE(result);
    CHECK(fills(s.face({0, 0, 1}), rects[0]));
}

TEST_CASE("Apply tool Hotspot mode applies the material and fits it in one step") {
    Scene s;
    const Rects rects{{0, 0, 512, 512}, {0, 512, 512, 1024, false, false, true}};
    hotspots(s, "trim/sheet", rects);
    auto& tool = dynamic_cast<ApplyTool&>(s.session.activeToolObject());
    auto readout = tool.readout();
    REQUIRE_FALSE(readout.parameters.empty());
    CHECK(readout.parameters[0].id == "mode");
    CHECK(readout.parameters[0].choices == std::vector<std::string>{"Material", "Hotspot"});
    REQUIRE(tool.setParameter("mode", 1));
    CHECK(tool.hotspot());
    CHECK(tool.readout().status.find("Hotspot") != std::string::npos);

    const auto top = Scene::ray({0, 0, 1});
    const auto apply = click(PointerButton::Right); // Hammer preset: right click applies.
    auto altApply = apply;
    altApply.alt = true;
    // The active material has no rects: nothing changes, and the readout says why.
    const auto undo = s.doc.undoCount();
    REQUIRE(s.session.input(apply, view(top)));
    CHECK(s.doc.undoCount() == undo);
    CHECK(tool.readout().status.find("no hotspot rects") != std::string::npos);

    s.session.setNewSolidMaterial("trim/sheet");
    REQUIRE(s.session.input(apply, view(top)));
    CHECK(s.doc.undoCount() == undo + 1);
    CHECK(s.doc.undoLabels().back() == "Apply hotspot");
    CHECK(s.face({0, 0, 1}).face.material == "trim/sheet");
    CHECK(fills(s.face({0, 0, 1}), rects[0]));
    REQUIRE(s.session.input(altApply, view(top)));
    CHECK(fills(s.face({0, 0, 1}), rects[1]));
    REQUIRE(s.doc.undo());
    REQUIRE(s.doc.undo());
    CHECK(s.face({0, 0, 1}).face.material == "apex/base");

    REQUIRE(tool.setParameter("mode", 0));
    CHECK_FALSE(tool.hotspot());
}
