#include "fixtures.hpp"
#include "editor/editor_session.hpp"

using namespace editor_test;

namespace {

const DVec3 viewDir = glm::normalize(DVec3{-0.6, -0.7, -0.5});

struct Scene {
    Document doc{parsed()};
    EditorSession session;
    NodeHandle a, b;
    NodeId aId = 0, bId = 0;
    int notifications = 0;

    Scene(bool both = true) {
        a = addBox(doc, {5, 5, 5}, {69, 69, 69});
        b = addBox(doc, {101, 5, 5}, {133, 37, 37});
        aId = id(doc, a);
        bId = id(doc, b);
        session.attach(doc);
        session.perform("selection.mode.solids");
        session.selectNode(aId);
        if (both) session.selectNode(bId, {.shift = true});
        doc.observe([this](const ChangeSet&) { ++notifications; });
    }
};

GizmoView viewFor(const GizmoContext& c) {
    GizmoView v;
    v.cameraPosition = c.pivot - viewDir * 2000.0;
    return v;
}
Ray rayTo(DVec3 point) { return {point - viewDir * 2000.0, viewDir}; }

DVec3 anchor(const GizmoPrimitive& p) {
    if (p.shape == GizmoShape::Segment) return p.points[0] * 0.4 + p.points[1] * 0.6;
    if (p.shape == GizmoShape::Quad) return (p.points[0] + p.points[2]) * 0.5;
    return p.centre + glm::normalize(glm::cross(p.normal, DVec3{1, 2, 3})) * p.radius;
}
GizmoPrimitive primitive(EditorSession& s, GizmoHandle handle) {
    const auto c = s.gizmoContext();
    REQUIRE(c);
    for (const auto& p : gizmoDescription(s.gizmoMode(), *c, viewFor(*c)))
        if (p.handle == handle) return p;
    FAIL("missing handle");
    return {};
}

std::vector<DVec3> vertices(const Document& doc, NodeHandle h) {
    const auto solid = geometry::solidGeometry(*doc.get(h)->block);
    REQUIRE(solid);
    return solid->poly.vertices;
}
void checkVertices(const Document& doc, NodeHandle h, std::vector<DVec3> expected) {
    const auto actual = vertices(doc, h);
    REQUIRE(actual.size() == expected.size());
    for (const auto point : expected) {
        CHECK(std::ranges::any_of(actual, [&](DVec3 p) { return glm::length(p - point) < 1e-6; }));
    }
}
std::vector<DVec3> boxCorners(DVec3 min, DVec3 max) {
    std::vector<DVec3> r;
    for (int i = 0; i < 8; ++i)
        r.push_back({i & 1 ? max.x : min.x, i & 2 ? max.y : min.y, i & 4 ? max.z : min.z});
    return r;
}
bool typeKeys(EditorSession& s, std::string_view keys) {
    for (const char c : keys) {
        if (s.modalKey(std::string(1, c)) != ModalEvent::Updated) return false;
    }
    return true;
}

} // namespace

TEST_CASE("session modal G X 32 Enter and the equivalent gizmo drag commit identical bytes") {
    Scene modalScene, dragScene;
    const auto before = modalScene.doc.write().value();
    REQUIRE(dragScene.doc.write().value() == before);
    const auto context = *modalScene.session.gizmoContext();
    checkPoint(context.pivot, {69, 37, 37});

    REQUIRE(modalScene.session.beginModal(GizmoMode::Translate, viewFor(context), rayTo(context.pivot)));
    CHECK(modalScene.session.transformActive());
    CHECK(modalScene.session.modalKey("X") == ModalEvent::Updated);
    REQUIRE(typeKeys(modalScene.session, "32"));
    CHECK(modalScene.session.modalKey("Enter") == ModalEvent::Commit);
    CHECK_FALSE(modalScene.session.transformActive());

    dragScene.session.setSnapMode(SnapMode::Relative);
    dragScene.session.setGridSize(16);
    const auto handle = primitive(dragScene.session, GizmoHandle::X);
    const auto start = anchor(handle);
    REQUIRE(dragScene.session.beginGizmoDrag(GizmoHandle::X, viewFor(context), rayTo(start)));
    CHECK(dragScene.session.updateGizmoDrag(rayTo(start + DVec3{31, 0, 0})));
    CHECK(dragScene.session.endGizmoDrag());

    CHECK(modalScene.doc.undoCount() == dragScene.doc.undoCount());
    CHECK(modalScene.doc.write().value() == dragScene.doc.write().value());
    CHECK(modalScene.doc.write().value() != before);
    checkVertices(modalScene.doc, modalScene.a, boxCorners({37, 5, 5}, {101, 69, 69}));
    CHECK(modalScene.notifications == 1);
    CHECK(modalScene.session.preview().empty());
    // One undo restores the exact original bytes.
    CHECK(modalScene.session.perform("edit.undo"));
    CHECK(modalScene.doc.write().value() == before);
}

TEST_CASE("session preview positions equal committed positions for every gizmo mode") {
    for (const auto mode : {GizmoMode::Translate, GizmoMode::Rotate, GizmoMode::Scale}) {
        Scene scene;
        scene.session.setGizmoMode(mode);
        const auto oldA = geometry::solidGeometry(*scene.doc.get(scene.a)->block).value();
        const auto oldB = geometry::solidGeometry(*scene.doc.get(scene.b)->block).value();
        const auto context = *scene.session.gizmoContext();
        const auto handle = mode == GizmoMode::Rotate ? GizmoHandle::Z : GizmoHandle::X;
        const auto p = primitive(scene.session, handle);
        const auto start = anchor(p);
        REQUIRE(scene.session.beginGizmoDrag(handle, viewFor(context), rayTo(start)));
        CHECK(scene.session.preview().transforms().size() == 2);
        DVec3 target = start + DVec3{40, 0, 0};
        if (mode == GizmoMode::Rotate) {
            const auto r = start - context.pivot;
            target = context.pivot + DVec3{-r.y, r.x, 0} * 0.7 + r * 0.7; // About 45 degrees.
        }
        CHECK(scene.session.updateGizmoDrag(rayTo(target), {.suspend = true}));
        const auto preview = scene.session.preview().transforms();
        const auto& readout = scene.session.activeReadout();
        REQUIRE(preview.size() == 2);
        CHECK(preview.at(scene.aId) == readout.transform.matrix());
        // Cancel path first: nothing written.
        const auto bytes = scene.doc.write().value();
        std::vector<std::vector<DVec3>> predicted;
        for (const auto* old : {&oldA, &oldB}) {
            std::vector<DVec3> list;
            for (const auto v : old->poly.vertices) list.push_back(DVec3{preview.at(old == &oldA ? scene.aId : scene.bId) * DVec4{v, 1.0}});
            predicted.push_back(list);
        }
        REQUIRE(scene.session.endGizmoDrag());
        CHECK(scene.session.preview().empty());
        checkVertices(scene.doc, scene.a, predicted[0]);
        checkVertices(scene.doc, scene.b, predicted[1]);
        CHECK(scene.doc.write().value() != bytes);
        CHECK(scene.doc.undoCount() == 3);
    }
}

TEST_CASE("session drag and modal cancel leave bytes history and observers untouched") {
    Scene scene;
    const auto bytes = scene.doc.write().value();
    const auto history = scene.doc.undoCount();
    const auto context = *scene.session.gizmoContext();
    const auto start = anchor(primitive(scene.session, GizmoHandle::X));
    REQUIRE(scene.session.beginGizmoDrag(GizmoHandle::X, viewFor(context), rayTo(start)));
    scene.session.updateGizmoDrag(rayTo(start + DVec3{48, 0, 0}));
    CHECK_FALSE(scene.session.preview().empty());
    scene.session.cancelTransform();
    CHECK(scene.session.preview().empty());
    CHECK_FALSE(scene.session.transformActive());
    CHECK_FALSE(scene.session.endGizmoDrag());

    for (const char* key : {"Escape", "RightClick"}) {
        REQUIRE(scene.session.beginModal(GizmoMode::Translate, viewFor(context), rayTo(context.pivot)));
        scene.session.modalKey("X");
        typeKeys(scene.session, "16");
        CHECK(scene.session.modalKey(key) == ModalEvent::Cancel);
        CHECK(scene.session.preview().empty());
        CHECK_FALSE(scene.session.transformActive());
    }
    CHECK(scene.doc.write().value() == bytes);
    CHECK(scene.doc.undoCount() == history);
    CHECK(scene.notifications == 0);
    // A zero-length drag commits nothing.
    REQUIRE(scene.session.beginGizmoDrag(GizmoHandle::X, viewFor(context), rayTo(start)));
    CHECK(scene.session.endGizmoDrag());
    CHECK(scene.doc.undoCount() == history);
}

TEST_CASE("session Escape cancels a drag before unpinning and before clearing the selection") {
    Scene scene;
    scene.session.pinAxisWorkPlane(Axis::Z, {0, 0, 0});
    REQUIRE(scene.session.workPlane().pinned());
    const auto context = *scene.session.gizmoContext();
    REQUIRE(scene.session.beginModal(GizmoMode::Translate, viewFor(context), rayTo(context.pivot)));
    CHECK(scene.session.perform("selection.none"));
    CHECK_FALSE(scene.session.transformActive());
    CHECK(scene.session.preview().empty());
    CHECK(scene.session.workPlane().pinned());
    CHECK_FALSE(scene.session.selection().ordered().empty());
    CHECK(scene.session.perform("selection.none"));
    CHECK_FALSE(scene.session.workPlane().pinned());
    CHECK_FALSE(scene.session.selection().ordered().empty());
    CHECK(scene.session.perform("selection.none"));
    CHECK(scene.session.selection().ordered().empty());
    // Same ordering for a gizmo drag.
    scene.session.selectNode(scene.aId);
    const auto ctx = *scene.session.gizmoContext();
    const auto start = anchor(primitive(scene.session, GizmoHandle::X));
    REQUIRE(scene.session.beginGizmoDrag(GizmoHandle::X, viewFor(ctx), rayTo(start)));
    scene.session.perform("selection.none");
    CHECK_FALSE(scene.session.transformActive());
    CHECK_FALSE(scene.session.selection().ordered().empty());
}

TEST_CASE("session undo during a drag aborts it") {
    Scene scene;
    scene.session.nudgeSelection(false, 1);
    const auto context = *scene.session.gizmoContext();
    REQUIRE(scene.session.beginModal(GizmoMode::Translate, viewFor(context), rayTo(context.pivot)));
    scene.session.perform("edit.undo");
    CHECK_FALSE(scene.session.transformActive());
    CHECK(scene.session.preview().empty());
}

TEST_CASE("session flips mirror about the pivot with valid windings and a single undo") {
    for (const auto axis : {Axis::X, Axis::Y, Axis::Z}) {
        Scene scene;
        scene.session.selectNode(scene.aId); // Replace: just box A.
        const auto bytes = scene.doc.write().value();
        const auto history = scene.doc.undoCount();
        const char* actions[] = {"edit.flip.x", "edit.flip.y", "edit.flip.z"};
        REQUIRE(scene.session.perform(actions[static_cast<int>(axis)]));
        CHECK(scene.doc.undoCount() == history + 1);
        CHECK(scene.notifications == 1);
        // A symmetric box mirrors onto itself; the pivot is its centre.
        checkVertices(scene.doc, scene.a, boxCorners({5, 5, 5}, {69, 69, 69}));
        const auto solid = geometry::solidGeometry(*scene.doc.get(scene.a)->block);
        REQUIRE(solid);
        CHECK(solid->poly.isValid());
        CHECK(solid->poly.validate().empty());
        CHECK(scene.session.perform("edit.undo"));
        CHECK(scene.doc.write().value() == bytes);
    }
    // An asymmetric pair mirrors its relative placement.
    Scene scene;
    REQUIRE(scene.session.perform("edit.flip.x"));
    checkVertices(scene.doc, scene.a, boxCorners({69, 5, 5}, {133, 69, 69}));
    checkVertices(scene.doc, scene.b, boxCorners({5, 5, 5}, {37, 37, 37}));
    for (const auto h : {scene.a, scene.b}) {
        const auto solid = geometry::solidGeometry(*scene.doc.get(h)->block);
        REQUIRE(solid);
        CHECK(solid->poly.validate().empty());
    }
}

TEST_CASE("session nudge moves one grid step along the work plane axes") {
    Scene scene(false);
    scene.session.setGridSize(16);
    const auto history = scene.doc.undoCount();
    REQUIRE(scene.session.perform("nudge.right"));
    checkVertices(scene.doc, scene.a, boxCorners({21, 5, 5}, {85, 69, 69}));
    REQUIRE(scene.session.perform("nudge.up"));
    checkVertices(scene.doc, scene.a, boxCorners({21, 21, 5}, {85, 85, 69}));
    REQUIRE(scene.session.perform("nudge.left"));
    REQUIRE(scene.session.perform("nudge.down"));
    checkVertices(scene.doc, scene.a, boxCorners({5, 5, 5}, {69, 69, 69}));
    CHECK(scene.doc.undoCount() == history + 4);
    // A pinned work plane supplies the axes.
    scene.session.pinAxisWorkPlane(Axis::X, {0, 0, 0});
    const auto resolved = scene.session.resolvedWorkPlane();
    REQUIRE(resolved);
    REQUIRE(scene.session.perform("nudge.right"));
    const auto moved = vertices(scene.doc, scene.a);
    const auto delta = resolved->plane.uAxis * 16.0;
    CHECK(std::ranges::any_of(moved, [&](DVec3 p) { return glm::length(p - (DVec3{5, 5, 5} + delta)) < 1e-6; }));
    // Nothing selected: no-op.
    scene.session.perform("selection.none");
    scene.session.perform("selection.none");
    scene.session.perform("selection.none");
    CHECK_FALSE(scene.session.perform("nudge.left"));
}

TEST_CASE("session transform dialog applies typed values as single transactions") {
    Scene scene;
    const auto bytes = scene.doc.write().value();
    const auto history = scene.doc.undoCount();
    REQUIRE(scene.session.moveSelection({10, 20, 30}, true));
    checkVertices(scene.doc, scene.a, boxCorners({15, 25, 35}, {79, 89, 99}));
    CHECK(scene.doc.undoCount() == history + 1);
    // Absolute: the pivot (bounds centre) lands on the typed point.
    REQUIRE(scene.session.moveSelection({0, 0, 0}, false));
    checkVertices(scene.doc, scene.a, boxCorners({-64, -32, -32}, {0, 32, 32}));
    CHECK(scene.session.perform("edit.undo"));
    CHECK(scene.session.perform("edit.undo"));
    CHECK(scene.doc.write().value() == bytes);

    REQUIRE(scene.session.rotateSelection({0, 0, 90}));
    checkVertices(scene.doc, scene.a, boxCorners({37, -27, 5}, {101, 37, 69}));
    CHECK(scene.session.perform("edit.undo"));
    REQUIRE(scene.session.scaleSelection({2, 1, 1}));
    CHECK(scene.session.perform("edit.undo"));
    CHECK(scene.doc.write().value() == bytes);
    CHECK_FALSE(scene.session.scaleSelection({0, 1, 1}));
    CHECK_FALSE(scene.session.scaleSelection({std::nan(""), 1, 1}));
    CHECK(scene.doc.write().value() == bytes);
    CHECK(scene.doc.undoCount() == history);
    CHECK(scene.session.moveSelection({1, 0, 0}, true));
}

TEST_CASE("session tooling toggles pivot space and snap state") {
    Scene scene;
    CHECK(scene.session.gizmoMode() == GizmoMode::Translate);
    scene.session.perform("gizmo.mode.rotate");
    CHECK(scene.session.gizmoMode() == GizmoMode::Rotate);
    scene.session.perform("gizmo.mode.scale");
    CHECK(scene.session.gizmoMode() == GizmoMode::Scale);
    scene.session.perform("gizmo.space.toggle");
    CHECK(scene.session.gizmoSpace() == GizmoSpace::Local);
    scene.session.perform("snap.mode.toggle");
    CHECK(scene.session.snapSettings().mode == SnapMode::Relative);
    scene.session.perform("transform.texture_lock.toggle");
    CHECK_FALSE(scene.session.textureLock());
    CHECK(scene.session.gizmoContext()->snap.mode == SnapMode::Relative);
    CHECK(scene.session.gizmoContext()->snap.gridSize == scene.session.gridSize());
    // Pivot cycles bounds centre -> active origin -> custom.
    checkPoint(scene.session.gizmoContext()->pivot, {69, 37, 37});
    scene.session.perform("gizmo.pivot.cycle");
    CHECK(scene.session.pivotMode() == PivotMode::ActiveOrigin);
    checkPoint(scene.session.gizmoContext()->pivot, {117, 21, 21}); // Box B is active.
    scene.session.perform("gizmo.pivot.cycle");
    scene.session.setCustomPivot({1, 2, 3});
    checkPoint(scene.session.gizmoContext()->pivot, {1, 2, 3});
    scene.session.perform("gizmo.pivot.cycle");
    CHECK(scene.session.pivotMode() == PivotMode::BoundsCentre);
    // Mode changes are ignored mid-drag.
    const auto context = *scene.session.gizmoContext();
    REQUIRE(scene.session.beginModal(GizmoMode::Translate, viewFor(context), rayTo(context.pivot)));
    scene.session.setGizmoMode(GizmoMode::Rotate);
    CHECK(scene.session.gizmoMode() == GizmoMode::Scale);
    CHECK_FALSE(scene.session.beginModal(GizmoMode::Rotate, viewFor(context), rayTo(context.pivot)));
    scene.session.cancelTransform();
    // Unhandled UI actions report false.
    CHECK_FALSE(scene.session.perform("transform.dialog"));
    CHECK_FALSE(scene.session.perform("snap.suspend"));
}

TEST_CASE("session local frame follows the active entity and face mode disables transforms") {
    Document doc(parsed());
    const auto entity = addBlock(doc, "entity { \"classname\" \"info_target\" \"origin\" \"10 20 30\" \"angles\" \"0 90 0\" }");
    const auto nodeId = id(doc, entity);
    EditorSession session;
    session.attach(doc);
    session.perform("selection.mode.objects");
    session.selectNode(nodeId);
    session.perform("gizmo.space.toggle");
    session.perform("gizmo.pivot.cycle");
    const auto context = session.gizmoContext();
    REQUIRE(context);
    checkPoint(context->pivot, {10, 20, 30});
    // Yaw 90 turns the entity's forward axis onto +Y; the right-handed local X is forward.
    checkPoint(context->frame[0], {0, 1, 0});
    checkPoint(context->frame[1], {-1, 0, 0});
    checkPoint(context->frame[2], {0, 0, 1});
    session.perform("selection.mode.faces");
    CHECK_FALSE(session.gizmoContext());
    CHECK_FALSE(session.flipSelection(Axis::X));
}

TEST_CASE("session 2000 solid drag publishes previews without touching the document") {
    Document doc(parsed());
    {
        auto tx = doc.begin("bulk");
        for (int i = 0; i < 2000; ++i) {
            const DVec3 min{double(i % 50) * 80, double(i / 50) * 80, 0};
            tx.create(solidBlock(geometry_test::build(geometry_test::boxSides(min, min + DVec3{64}))));
        }
        tx.commit();
    }
    EditorSession session;
    session.attach(doc);
    session.perform("selection.mode.solids");
    session.perform("selection.all");
    REQUIRE(session.selection().ordered().size() == 2000);
    const auto history = doc.undoCount();
    const auto bytes = doc.write().value();
    int notifications = 0;
    doc.observe([&](const ChangeSet&) { ++notifications; });

    const auto context = *session.gizmoContext();
    const auto start = anchor(primitive(session, GizmoHandle::X));
    REQUIRE(session.beginGizmoDrag(GizmoHandle::X, viewFor(context), rayTo(start)));
    CHECK(session.preview().transforms().size() == 2000);
    const auto revision = session.preview().revision();
    for (int i = 1; i <= 5; ++i) {
        session.updateGizmoDrag(rayTo(start + DVec3{16.0 * i, 0, 0}), {.suspend = true});
        CHECK(session.preview().revision() == revision + static_cast<std::uint64_t>(i));
    }
    // Identical input is not a new revision.
    session.updateGizmoDrag(rayTo(start + DVec3{80, 0, 0}), {.suspend = true});
    CHECK(session.preview().revision() == revision + 5);
    CHECK(notifications == 0);
    CHECK(doc.undoCount() == history);
    CHECK(doc.write().value() == bytes);

    REQUIRE(session.endGizmoDrag());
    CHECK(session.preview().empty());
    CHECK(notifications == 1);
    CHECK(doc.undoCount() == history + 1);
    CHECK(session.perform("edit.undo"));
    CHECK(doc.write().value() == bytes);
}

TEST_CASE("session pivots a single object on its origin unless that preference is off") {
    Document doc(parsed());
    const auto entity = addBlock(doc, "entity { \"classname\" \"prop_static\" \"model\" \"models/x.mdl\" \"origin\" \"10 20 30\" }");
    EditorSession session;
    // A model whose bounds sit well off its origin, like a prop modelled away from (0, 0, 0).
    session.setEntityBoundsProvider([](const kv1::Node&) {
        return std::optional<EntityPickShape>(EntityPickShape{nullptr, 0, LocalBox{{0, 0, 0}, {64, 64, 64}}});
    });
    session.attach(doc);
    session.perform("selection.mode.objects");
    session.selectNode(id(doc, entity));
    REQUIRE(session.pivotAtObjectOrigin);
    checkPoint(session.gizmoContext()->pivot, {10, 20, 30}); // Hammer: the entity origin.
    session.pivotAtObjectOrigin = false;
    checkPoint(session.gizmoContext()->pivot, {42, 52, 62}); // Bounds centre.
    session.pivotAtObjectOrigin = true;
    session.perform("gizmo.pivot.cycle"); // Active origin stays the origin either way.
    checkPoint(session.gizmoContext()->pivot, {10, 20, 30});
}

TEST_CASE("session pivots several objects on their shared bounds centre") {
    Scene scene;
    REQUIRE(scene.session.pivotAtObjectOrigin);
    checkPoint(scene.session.gizmoContext()->pivot, {69, 37, 37});
}
