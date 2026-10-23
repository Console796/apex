#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/snapper.hpp"
#include "editor/tool.hpp"

using namespace editor_test;

namespace {

ToolView topView(bool orthographic = true, double zoom = 1.0) {
    ToolView view;
    view.size = {1000, 1000};
    view.cameraPosition = {0, 0, 200.0 / zoom};
    view.cameraForward = {0, 0, -1};
    view.cameraUp = {0, 1, 0};
    view.orthographic = orthographic;
    view.orthoHeight = 200.0 / zoom;
    view.fovYDegrees = 90;
    return view;
}

Ray toward(const ToolView& view, DVec3 point) {
    return view.orthographic ? Ray{{point.x, point.y, view.cameraPosition.z}, view.cameraForward} :
        Ray{view.cameraPosition, glm::normalize(point - view.cameraPosition)};
}

const SurfacePlane ground{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {}};

SnapResult snap(const Document& doc, const PickIndex& index, Ray ray, const ToolView& view,
                bool suspended = false) {
    WorkPlaneState plane;
    SnapSettings settings;
    settings.gridSize = 16;
    return Snapper(doc, index, plane, settings, ground).snap(ray, view, {suspended});
}

} // namespace

TEST_CASE("snapper vertex beats edge and grid and midpoint beats closest point") {
    Document doc(parsed());
    const auto solid = addBox(doc, {-50, -50, -20}, {50, 50, 0});
    PickIndex index(doc);
    const auto view = topView();
    const auto vertex = snap(doc, index, toward(view, {49, 49, 0}), view);
    CHECK(vertex.kind == SnapKind::Vertex);
    CHECK(vertex.node == id(doc, solid));
    CHECK(vertex.side.has_value());
    checkPoint(vertex.point, {50, 50, 0});
    const auto midpoint = snap(doc, index, toward(view, {1, 49, 0}), view);
    CHECK(midpoint.kind == SnapKind::EdgeMidpoint);
    checkPoint(midpoint.point, {0, 50, 0});
    const auto edge = snap(doc, index, toward(view, {20, 49, 0}), view);
    CHECK(edge.kind == SnapKind::Edge);
    checkPoint(edge.point, {20, 50, 0});
    const auto grid = snap(doc, index, toward(view, {20, 47, 0}), view);
    CHECK(grid.kind == SnapKind::Grid);
    checkPoint(grid.point, {16, 48, 0});
}

TEST_CASE("snapper radius is eight pixels at two zooms in both projections") {
    Document doc(parsed());
    addBox(doc, {-50, -50, -20}, {50, 50, 0});
    PickIndex index(doc);
    for (const bool orthographic : {true, false}) {
        for (const double zoom : {1.0, 2.0}) {
            const auto view = topView(orthographic, zoom);
            const double pixelsPerUnit = orthographic ? 5.0 * zoom : 2.5 * zoom;
            CAPTURE(orthographic);
            CAPTURE(zoom);
            // Same solid and camera direction: offsets are specified in cursor pixels.
            const auto nearVertex = snap(doc, index, toward(view,
                {50 - 5 / pixelsPerUnit, 50 - 5 / pixelsPerUnit, 0}), view);
            CHECK(nearVertex.kind == SnapKind::Vertex);
            checkPoint(nearVertex.point, {50, 50, 0});
            const auto nearEdge = snap(doc, index, toward(view, {20, 50 - 7 / pixelsPerUnit, 0}), view);
            CHECK(nearEdge.kind == SnapKind::Edge);
            const auto outside = snap(doc, index, toward(view, {20, 50 - 9 / pixelsPerUnit, 0}), view);
            CHECK(outside.kind == SnapKind::Grid);
        }
    }
}

TEST_CASE("snapper suspend returns raw surface and hidden solids supply no features") {
    Document doc(parsed());
    const auto solid = addBox(doc, {-50, -50, -20}, {50, 50, 0});
    PickIndex index(doc);
    const auto view = topView();
    const auto raw = snap(doc, index, toward(view, {49, 49, 0}), view, true);
    CHECK(raw.kind == SnapKind::Surface);
    CHECK(raw.node == id(doc, solid));
    checkPoint(raw.point, {49, 49, 0});
    ViewState hidden;
    hidden.isolate.emplace();
    index.setViewState(hidden);
    const auto result = snap(doc, index, toward(view, {49, 49, 0}), view);
    CHECK(result.kind == SnapKind::Grid);
    CHECK_FALSE(result.node);
    const auto features = index.nearestFeature(toward(view, {49, 49, 0}), 8, view);
    CHECK_FALSE(features.vertex);
    CHECK_FALSE(features.midpoint);
    CHECK_FALSE(features.edge);
    index.setViewState({});
    const auto wrapper = addBlock(doc, "hidden {}");
    auto tx = doc.begin("hide solid");
    tx.reparent(solid, wrapper);
    index.apply(tx.commit());
    const auto documentHidden = snap(doc, index, toward(view, {49, 49, 0}), view);
    CHECK(documentHidden.kind == SnapKind::Grid);
    CHECK_FALSE(documentHidden.node);
}

TEST_CASE("snapper rejects occluded features and prefers frontmost ties") {
    Document doc(parsed());
    const auto front = addBox(doc, {-50, -50, -20}, {50, 50, 0});
    addBox(doc, {-49, -49, -40}, {49, 49, -30});
    PickIndex index(doc);
    const auto view = topView();
    const auto result = snap(doc, index, toward(view, {49, 49, 0}), view);
    CHECK(result.kind == SnapKind::Vertex);
    CHECK(result.node == id(doc, front));
    checkPoint(result.point, {50, 50, 0});
    const auto occluded = index.nearestFeature(toward(view, {49, 49, 0}), 0.1, view);
    CHECK_FALSE(occluded.vertex);
    CHECK_FALSE(occluded.edge);
}

TEST_CASE("snapper grid stays in sloped surface frame and uses absolute mode") {
    Document doc(parsed());
    auto tx = doc.begin("slope");
    auto sides = geometry_test::boxSides({-50, -50, -50}, {50, 50, 50});
    sides.push_back(geometry_test::side(glm::normalize(DVec3{1, 0, 1}), 32, 7));
    const auto solid = tx.create(solidBlock(geometry_test::build(sides)));
    tx.commit();
    const auto geometry = geometry::solidGeometry(*doc.get(solid)->block);
    REQUIRE(geometry);
    for (const auto& face : geometry->poly.faces) {
        if (std::abs(face.plane.normal.z) < 0.1 || std::abs(face.plane.normal.z) > 0.99) continue;
        const auto plane = planeFromFace(geometry->poly, face);
        REQUIRE(plane);
        const auto raw = plane->origin + plane->uAxis * 0.7 + plane->vAxis * 0.3;
        ToolView view;
        view.size = {1000, 1000};
        view.cameraPosition = raw + plane->normal * 100.0;
        view.cameraForward = -plane->normal;
        view.cameraUp = plane->vAxis;
        view.orthographic = true;
        view.orthoHeight = 1; // Keep all boundary features outside the pixel radius.
        PickIndex index(doc);
        WorkPlaneState state;
        SnapSettings settings;
        settings.mode = SnapMode::Relative;
        settings.gridSize = 1;
        const auto result = Snapper(doc, index, state, settings, ground).snap(
            {view.cameraPosition, view.cameraForward}, view);
        CHECK(result.kind == SnapKind::Grid);
        CHECK(result.node == id(doc, solid));
        CHECK(std::abs(face.plane.signedDistance(result.point)) < 1e-6);
        checkPoint(result.point, plane->origin + plane->uAxis);
        return;
    }
    FAIL("fixture has no sloped face");
}

TEST_CASE("snapper work plane pin, fallback, parallel ray, and session forwarding") {
    Document doc(parsed());
    PickIndex index(doc);
    WorkPlaneState state;
    state.pinAxisPlane(Axis::Z, {0, 0, 10});
    SnapSettings settings;
    settings.gridSize = 16;
    const auto view = topView();
    const auto ray = toward(view, {19, 23, 0});
    const auto pinned = Snapper(doc, index, state, settings, ground).snap(ray, view);
    CHECK(pinned.kind == SnapKind::Grid);
    checkPoint(pinned.point, {16, 16, 10});
    const auto raw = Snapper(doc, index, state, settings, ground).snap(ray, view, {true});
    checkPoint(raw.point, {19, 23, 10});
    CHECK(Snapper(doc, index, state, settings, ground).snap({{0, 0, 20}, {1, 0, 0}}, view).kind == SnapKind::None);
    state.unpin();
    const auto fallback = Snapper(doc, index, state, settings, ground).snap(ray, view);
    checkPoint(fallback.point, {16, 16, 0});
    EditorSession session;
    CHECK(session.snap(ray, view).kind == SnapKind::None);
    session.attach(doc);
    session.setGridSize(16);
    checkPoint(session.snap(ray, view).point, fallback.point);
    checkPoint(session.snap(ray, view, true).point, {19, 23, 0});
}

TEST_CASE("snapper projection rejects behind-camera points and uses logical pixel axes") {
    for (const bool orthographic : {true, false}) {
        const auto view = topView(orthographic);
        const auto center = worldToPixel(view, {0, 0, 0});
        REQUIRE(center);
        CHECK(glm::length(*center - DVec2{500, 500}) < 1e-6);
        CHECK(worldToPixel(view, {0, 0, 201}).has_value() == orthographic);
        const auto offset = worldToPixel(view, {10, 20, 0});
        REQUIRE(offset);
        CHECK(offset->x > center->x);
        CHECK(offset->y < center->y);
    }
}

TEST_CASE("snapper equal pixel distances prefer depth before node identity") {
    Document doc(parsed());
    addBox(doc, {52, 50, -50}, {70, 70, -30});
    const auto front = addBox(doc, {-50, -50, -20}, {50, 50, 0});
    PickIndex index(doc);
    const auto view = topView();
    const auto features = index.nearestFeature(toward(view, {51, 50, 0}), 8, view);
    REQUIRE(features.vertex);
    CHECK(features.vertex->node == id(doc, front));
    checkPoint(features.vertex->point, {50, 50, 0});
}

TEST_CASE("snapper perspective closest edge point interpolates world depth") {
    Document doc(parsed());
    auto sides = geometry_test::boxSides({-50, -50, -50}, {50, 50, 50});
    sides.push_back(geometry_test::side(glm::normalize(DVec3{1, 0, 1}), 32, 7));
    auto tx = doc.begin("slope");
    tx.create(solidBlock(geometry_test::build(sides)));
    tx.commit();
    PickIndex index(doc);
    const auto view = topView(false);
    const DVec3 target{20, 50, 32 * std::sqrt(2.0) - 20};
    const auto features = index.nearestFeature(toward(view, target), 8, view);
    REQUIRE(features.edge);
    checkPoint(features.edge->point, target);
    CHECK(features.edge->pixels < 1e-6);
}

TEST_CASE("snapper orthographic ray sees geometry behind camera position") {
    Document doc(parsed());
    addBox(doc, {-50, -50, -20}, {50, 50, 0});
    PickIndex index(doc);
    auto view = topView();
    view.cameraPosition.z = -100;
    const Ray ray{{49, 49, 1000}, {0, 0, -1}};
    const auto result = snap(doc, index, ray, view);
    CHECK(result.kind == SnapKind::Vertex);
    checkPoint(result.point, {50, 50, 0});
}
