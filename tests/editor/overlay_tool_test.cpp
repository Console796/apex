// Plan 0.8 D12: the Overlay tool places on a face, lists every face its footprint covers, edits
// the quad with handles (one undo step per drag) and takes faces in and out with Shift-click.
#include "face_fixtures.hpp"
#include "editor/overlay_placement.hpp"
#include "editor/overlay_tool.hpp"

#include <algorithm>
#include <cmath>

using namespace face_test;

namespace {

const char* kEmpty = "versioninfo\n{\n\"editorversion\" \"400\"\n}\nworld\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n}\n";

NodeHandle world(const Document& doc) {
    for (const auto r : doc.roots())
        if (doc.get(*doc.handle(r))->block->key() == "world") return *doc.handle(r);
    FAIL("no world");
    return {};
}

// Two floor boxes meeting at x = 0 and a wall standing at x = 64..80.
struct Floor {
    Document doc{parsed(kEmpty)};
    EditorSession session;
    NodeHandle left, right, wall;
    Floor() {
        const auto w = world(doc);
        left = addBox(doc, {-64, -64, -64}, {0, 64, 0}, w);
        right = addBox(doc, {0, -64, -64}, {64, 64, 0}, w);
        wall = addBox(doc, {64, -64, -64}, {80, 64, 64}, w);
        session.attach(doc);
        session.faceTextureSize = [](std::string_view) { return std::pair{128, 64}; };
        REQUIRE(session.perform("tool.overlay"));
    }
    SideId top(NodeHandle solid) const {
        const auto poly = geometry::solidGeometry(*doc.get(solid)->block);
        for (const auto& face : poly->poly.faces)
            if (face.plane.normal.z > 0.999) return static_cast<SideId>(face.sideId);
        FAIL("no top");
        return 0;
    }
    OverlayTool& tool() { return dynamic_cast<OverlayTool&>(session.activeToolObject()); }
    const kv1::Node& selected() const {
        const auto active = session.selection().active();
        REQUIRE(active);
        return *doc.get(*doc.handle(std::get<NodeId>(*active)))->block;
    }
    std::string key(std::string_view name) const {
        const auto* value = selected().find(name);
        return value ? value->decodedValue().value_or("") : "";
    }
    std::vector<int> sides() const {
        auto list = geometry::parseSideList(key("sides"));
        std::ranges::sort(list);
        return list;
    }
    geometry::OverlayQuad quad() const { return *geometry::parseOverlay(selected()); }
};

// A view straight down onto `point`, so the cursor sits on it in the middle of the view.
ToolView above(DVec3 point) { return view({point + DVec3{0, 0, 256}, {0, 0, -1}}); }
InputEvent at(InputKind kind, DVec3 point, bool shift = false) {
    auto event = click(PointerButton::Left, false, shift);
    event.kind = kind;
    event.left = kind != InputKind::Release;
    event.cursor = *worldToPixel(above(point), point);
    return event;
}
// Drags a handle from `from` to `to` (both on the floor's plane, z = 0).
void drag(Floor& f, DVec3 from, DVec3 to) {
    REQUIRE(f.session.input(at(InputKind::Press, from), above(from)));
    // The camera stays where the press was; the cursor ray moves.
    auto v = above(from);
    v.cursorRay = {to + DVec3{0, 0, 256}, {0, 0, -1}};
    auto move = at(InputKind::Move, from);
    move.cursor = *worldToPixel(v, to);
    f.session.input(move, v);
    f.session.input(at(InputKind::Release, to), v);
}

} // namespace

TEST_CASE("[overlay] Overlay tool places the active material sized from its aspect, on every face the quad covers") {
    Floor f;
    const auto undo = f.doc.undoCount();
    REQUIRE(f.session.input(at(InputKind::Press, {-8, 0, 0}), above({-8, 0, 0})));
    CHECK(f.doc.undoCount() == undo + 1);
    CHECK(f.key("classname") == "info_overlay");
    CHECK(f.key("material") == f.session.newSolidStyle().material);
    const auto quad = f.quad();
    CHECK(glm::length(quad.origin - DVec3{-8, 0, 0}) < 1e-9);
    CHECK(glm::length(quad.normal - DVec3{0, 0, 1}) < 1e-9);
    CHECK(glm::length(glm::cross(quad.normal, quad.basisU) - quad.basisV) < 1e-9);
    // A 128x64 texture at the face's scale of 1: the quad is 128 by 64.
    CHECK(geometry::quadArea(quad) == doctest::Approx(128.0 * 64.0));
    CHECK(f.key("origin") == f.key("BasisOrigin"));
    // It reaches across x = 0 onto the right box, but not onto the wall (edge-on, and its top is
    // out of reach).
    auto expected = std::vector<int>{static_cast<int>(f.top(f.left)), static_cast<int>(f.top(f.right))};
    std::ranges::sort(expected);
    CHECK(f.sides() == expected);
    const auto geometry = document::overlayGeometry(f.doc, f.selected());
    double area = 0.0;
    for (const auto& piece : geometry.pieces) area += geometry::fragmentArea(piece.fragment);
    CHECK(area == doctest::Approx(120.0 * 64.0)); // x -72..56: 8 units hang past the floor at x = -64.
    REQUIRE(f.doc.undo());
    CHECK(f.doc.undoCount() == undo);
}

TEST_CASE("[overlay] footprint faces stop at 45 degrees from the basis normal") {
    Document doc(parsed(kEmpty));
    const auto w = world(doc);
    addBox(doc, {-64, -64, -64}, {64, 64, 0}, w);
    document::SurfaceLookup lookup(doc);
    const auto surfaces = lookup.all();
    // A quad tilted 40 degrees from the floor reaches the top face; one tilted 50 degrees does not.
    const auto tilted = [&](double degrees) {
        const double r = glm::radians(degrees);
        return geometry::faceAlignedQuad({0, 0, 0}, {std::sin(r), 0, std::cos(r)}, {1, 0, 0}, {32, 32});
    };
    CHECK(editor::footprintSides(surfaces, tilted(40)).size() == 1);
    CHECK(editor::footprintSides(surfaces, tilted(50)).empty());
    CHECK(editor::toggleSide({3, 5}, 5) == std::vector<int>{3});
    CHECK(editor::toggleSide({3}, 7) == std::vector<int>{3, 7});
}

TEST_CASE("[overlay] Overlay tool handles move, scale, rotate and drag a corner, each drag one undo step") {
    Floor f;
    REQUIRE(f.session.input(at(InputKind::Press, {-8, 0, 0}), above({-8, 0, 0})));
    const auto start = f.quad();
    const auto handles = editor::overlayHandles(start);
    const auto spot = [&](OverlayHandleKind kind, int corner = 0) {
        for (const auto& h : handles)
            if (h.handle.kind == kind && h.handle.corner == corner) return h.position;
        FAIL("no handle");
        return DVec3{};
    };

    SUBCASE("move: the quad follows the cursor and picks up faces, in one undo step") {
        const auto undo = f.doc.undoCount();
        // Move it wholly onto the left box first: the right box's top leaves the list.
        drag(f, spot(OverlayHandleKind::Move), {-24, 0, 0});
        CHECK(f.doc.undoCount() == undo + 1);
        CHECK(glm::length(f.quad().origin - DVec3{-24, 0, 0}) < 1e-9);
        // Still reaching x = 40 at its +U edge: both boxes.
        CHECK(f.sides().size() == 2);
        drag(f, f.quad().origin, {-70, 0, 0}); // Spans x -134..-6: the left box only.
        CHECK(f.doc.undoCount() == undo + 2);
        CHECK(f.sides() == std::vector<int>{static_cast<int>(f.top(f.left))});
        REQUIRE(f.doc.undo());
        CHECK(glm::length(f.quad().origin - DVec3{-24, 0, 0}) < 1e-9);
    }
    SUBCASE("scale along U about the centre") {
        const auto from = spot(OverlayHandleKind::ScaleU);
        const auto to = start.origin + (from - start.origin) * 0.5;
        drag(f, from, to);
        CHECK(geometry::quadArea(f.quad()) == doctest::Approx(64.0 * 64.0));
        CHECK(f.quad().points[2].y == doctest::Approx(start.points[2].y));
    }
    SUBCASE("rotate a quarter turn about the normal") {
        const auto from = spot(OverlayHandleKind::Rotate);
        const auto arm = from - start.origin;
        drag(f, from, start.origin + start.basisV * glm::length(arm));
        CHECK(glm::length(f.quad().basisU - start.basisV) < 1e-6);
        CHECK(glm::length(f.quad().basisV + start.basisU) < 1e-6);
        CHECK(f.quad().points == start.points);
    }
    SUBCASE("a corner moves alone") {
        const auto from = spot(OverlayHandleKind::Corner, 2);
        drag(f, from, from + start.basisU * 8.0 + start.basisV * 4.0);
        CHECK(f.quad().points[2] == start.points[2] + DVec2{8, 4});
        CHECK(f.quad().points[0] == start.points[0]);
    }
    SUBCASE("Escape mid-drag puts the quad back") {
        const auto undo = f.doc.undoCount();
        REQUIRE(f.session.input(at(InputKind::Press, start.origin), above(start.origin)));
        auto v = above(start.origin);
        v.cursorRay = {DVec3{-40, 0, 256}, {0, 0, -1}};
        f.session.input(at(InputKind::Move, start.origin), v);
        CHECK_FALSE(f.session.preview().overlays().empty());
        InputEvent escape;
        escape.kind = InputKind::KeyDown;
        escape.key = "Escape";
        REQUIRE(f.session.input(escape, v));
        CHECK_FALSE(f.tool().dragging());
        CHECK(f.quad() == start);
        CHECK(f.doc.undoCount() == undo);
        CHECK(f.session.preview().empty());
    }
}

TEST_CASE("[overlay] a handle drag previews without writing and commits once, as the document would draw it") {
    Floor f;
    REQUIRE(f.session.input(at(InputKind::Press, {-8, 0, 0}), above({-8, 0, 0})));
    const auto start = f.quad();
    const auto startSides = f.sides();
    const auto node = std::get<NodeId>(*f.session.selection().active());
    const auto undo = f.doc.undoCount();
    const auto history = f.doc.historyState();
    std::size_t changes = 0;
    const auto token = f.doc.observe([&](const document::ChangeSet&) { ++changes; });

    REQUIRE(f.session.input(at(InputKind::Press, start.origin), above(start.origin)));
    CHECK(f.tool().dragging());
    auto v = above(start.origin);
    // The viewport cancels the pointer on every press a tool takes; that must not end the drag.
    auto cancelPointer = at(InputKind::Move, start.origin);
    cancelPointer.kind = InputKind::CancelPointer;
    f.session.input(cancelPointer, v);
    CHECK(f.tool().dragging());
    for (const double x : {-16.0, -40.0, -70.0}) {
        v.cursorRay = {DVec3{start.origin.x + x, 0, 256}, {0, 0, -1}};
        f.session.input(at(InputKind::Move, start.origin), v);
    }
    // Nothing reached the document during the drag: the preview carries it.
    CHECK(changes == 0);
    CHECK(f.doc.historyState() == history);
    CHECK(f.quad() == start);
    CHECK(f.sides() == startSides);
    const auto& preview = f.session.preview();
    REQUIRE(preview.overlays().contains(node));
    REQUIRE(preview.entityKeys().contains(node));
    const auto previewed = preview.overlays().at(node);
    const auto keys = preview.entityKeys().at(node);
    CHECK(previewed.quads.at(0).origin.x == doctest::Approx(start.origin.x - 70.0));

    f.session.input(at(InputKind::Release, start.origin), v);
    CHECK_FALSE(f.tool().dragging());
    CHECK(f.session.preview().empty());
    CHECK(changes == 1);
    CHECK(f.doc.undoCount() == undo + 1);
    CHECK(f.doc.undoLabels().back() == "Edit overlay");
    // The commit writes the keys the preview showed, and draws what the preview drew.
    for (const auto& [key, value] : keys) CHECK(f.key(key) == value);
    CHECK(f.sides() == std::vector<int>{static_cast<int>(f.top(f.left))});
    const auto committed = document::overlayGeometry(f.doc, f.selected());
    CHECK(committed.listed == previewed.listed);
    CHECK(committed.missing == previewed.missing);
    REQUIRE(committed.pieces.size() == previewed.pieces.size());
    for (std::size_t i = 0; i < committed.pieces.size(); ++i) {
        CHECK(committed.pieces[i].solid == previewed.pieces[i].solid);
        CHECK(geometry::fragmentArea(committed.pieces[i].fragment) ==
              doctest::Approx(geometry::fragmentArea(previewed.pieces[i].fragment)));
    }
    REQUIRE(f.doc.undo());
    CHECK(f.quad() == start);
    CHECK(f.sides() == startSides);
    f.doc.unobserve(token);
}

TEST_CASE("[overlay] the footprint's bounds cull keeps every face a full clip finds") {
    Document doc(parsed(kEmpty));
    const auto w = world(doc);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x)
            addBox(doc, {x * 32.0, y * 32.0, -32.0 - 4.0 * ((x + y) % 3)}, {x * 32.0 + 32.0, y * 32.0 + 32.0, 0.0 - 4.0 * ((x + y) % 3)}, w);
    document::SurfaceLookup lookup(doc);
    const auto surfaces = lookup.all();
    const auto everyFace = [&](const geometry::OverlayQuad& quad) {
        const double minCosine = std::cos(glm::radians(editor::kOverlayFootprintDegrees)) - 1e-9;
        std::vector<int> sides;
        for (const auto& surface : surfaces) {
            if (glm::dot(surface.normal, quad.normal) < minCosine) continue;
            const auto fragment = document::clipToSurface(quad, surface);
            if (fragment && geometry::fragmentArea(*fragment) > editor::kOverlayFootprintMinArea)
                sides.push_back(static_cast<int>(surface.side));
        }
        return sides;
    };
    // Square, rotated, tilted and skewed (one corner dragged) quads at a spread of places.
    for (int i = 0; i < 24; ++i) {
        const double r = glm::radians(15.0 * i);
        const DVec3 normal = glm::normalize(DVec3{0.3 * std::sin(r), 0.2 * std::cos(r), 1.0});
        auto quad = geometry::faceAlignedQuad({20.0 + 7.0 * i, 30.0 + 5.0 * (i % 7), 0.0}, normal,
                                              {std::cos(r), std::sin(r), 0.0}, {40.0 + 3.0 * i, 24.0});
        if (i % 3 == 0) quad.points[2] += DVec2{18.0, 9.0};
        const auto sides = editor::footprintSides(surfaces, quad);
        CHECK(sides == everyFace(quad));
        CHECK_FALSE(sides.empty());
    }
}

TEST_CASE("[overlay] Shift-click adds a face to the selected overlay's list and removes it again") {
    Floor f;
    REQUIRE(f.session.input(at(InputKind::Press, {-8, 0, 0}), above({-8, 0, 0})));
    const auto before = f.sides();
    const int wallTop = static_cast<int>(f.top(f.wall));
    const DVec3 onWall{72, 0, 64};
    REQUIRE(f.session.input(at(InputKind::Press, onWall, true), above(onWall)));
    const auto withWall = f.sides();
    CHECK(std::ranges::find(withWall, wallTop) != withWall.end());
    CHECK(f.sides().size() == before.size() + 1);
    REQUIRE(f.session.input(at(InputKind::Press, onWall, true), above(onWall)));
    CHECK(f.sides() == before);
}

TEST_CASE("[overlay] Decal mode places an infodecal with the active texture, labelled approximate") {
    Floor f;
    REQUIRE(f.session.activeToolObject().setParameter("mode", 1.0));
    CHECK(f.tool().mode() == OverlayTool::Mode::Decal);
    CHECK(f.session.activeToolObject().readout().status.find("pproximate") != std::string::npos);
    REQUIRE(f.session.input(at(InputKind::Press, {32, 0, 0}), above({32, 0, 0})));
    CHECK(f.key("classname") == "infodecal");
    CHECK(f.key("texture") == f.session.newSolidStyle().material);
    CHECK(f.key("origin") == "32 0 0");
    const auto geometry = document::overlayGeometry(f.doc, f.selected(), [](std::string_view) { return DVec2{32.0}; });
    CHECK(geometry.approximate);
    CHECK(geometry.pieces.size() == 1);
}
