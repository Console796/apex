#include <doctest/doctest.h>
#include "editor/compile_overlay.hpp"
#include "editor/diagnostic_lookup.hpp"
#include "fixtures.hpp"

using namespace apex;
using compile::ObjectKind;
using compile::ObjectRef;

namespace {
int vmfId(const document::Document& doc, document::NodeHandle handle) {
    return std::stoi(doc.get(handle)->block->find("id")->decodedValue().value());
}
}

TEST_CASE("diagnostic lookup finds brushes, sides and entities by VMF id") {
    document::Document doc(editor_test::parsed("world { id 1 classname worldspawn }"));
    const auto solid = editor_test::addBox(doc, DVec3{0.0}, DVec3{64.0});
    const auto entity = editor_test::addBlock(doc, "entity { id 20 classname info_target }");
    const auto group = editor_test::addBlock(doc, "group { id 30 }");
    const auto grouped = editor_test::addBox(doc, DVec3{100.0}, DVec3{164.0}, group);
    const int solidId = vmfId(doc, solid), entityId = vmfId(doc, entity), groupedId = vmfId(doc, grouped);

    CHECK(editor::findDiagnosticObject(doc, {ObjectKind::Brush, solidId}) == editor_test::id(doc, solid));
    CHECK(editor::findDiagnosticObject(doc, {ObjectKind::Entity, entityId}) == editor_test::id(doc, entity));
    CHECK(editor::findDiagnosticObject(doc, {ObjectKind::Brush, groupedId}) == editor_test::id(doc, grouped));
    // A brush id never matches an entity and the other way round.
    CHECK_FALSE(editor::findDiagnosticObject(doc, {ObjectKind::Entity, solidId}));
    CHECK_FALSE(editor::findDiagnosticObject(doc, {ObjectKind::Brush, entityId}));
    // The world is not an entity a diagnostic can name.
    CHECK_FALSE(editor::findDiagnosticObject(doc, {ObjectKind::Entity, 1}));
    CHECK_FALSE(editor::findDiagnosticObject(doc, {ObjectKind::Brush, 99999}));
    const auto sideId = doc.get(solid)->block->children();
    int firstSide = 0;
    for (const auto& child : sideId) if (child.key() == "side")
        { firstSide = std::stoi(child.find("id")->decodedValue().value()); break; }
    REQUIRE(firstSide != 0);
    CHECK(editor::findDiagnosticObject(doc, {ObjectKind::Side, firstSide}) == editor_test::id(doc, solid));
}

TEST_CASE("diagnostic lookup reports a deleted object as no longer in the map") {
    document::Document doc(editor_test::parsed("world { id 1 classname worldspawn }"));
    const auto solid = editor_test::addBox(doc, DVec3{0.0}, DVec3{64.0});
    const int solidId = vmfId(doc, solid);
    REQUIRE(editor::findDiagnosticObject(doc, {ObjectKind::Brush, solidId}));
    auto tx = doc.begin("delete");
    tx.destroy(solid);
    tx.commit();
    CHECK_FALSE(editor::findDiagnosticObject(doc, {ObjectKind::Brush, solidId}));
}

TEST_CASE("compile overlay primitives come from hand-computed data, grey when stale and filter by cluster") {
    editor::CompileOverlay overlay;
    overlay.leak = {{0, 0, 0}, {10, 0, 0}, {10, 20, 0}};
    overlay.portals.clusters = 3;
    overlay.portals.portals = {{0, 1, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}}},
                               {1, 2, {{0, 0, 5}, {1, 0, 5}, {1, 1, 5}, {0, 1, 5}}}};
    auto both = editor::compileOverlayPrimitives(overlay, false, true, true);
    // 2 leak segments + 3 + 4 portal edges, one start marker and its label.
    CHECK(both.worldSegments.size() == 9);
    REQUIRE(both.worldPoints.size() == 1);
    CHECK(both.worldPoints[0].position == DVec3(0, 0, 0));
    CHECK(both.labels.size() == 1);

    overlay.cluster = 2;
    CHECK(editor::compileOverlayPrimitives(overlay, false, true, true).worldSegments.size() == 6);
    overlay.cluster = 0;
    CHECK(editor::compileOverlayPrimitives(overlay, false, true, true).worldSegments.size() == 5);
    overlay.cluster.reset();

    CHECK(editor::compileOverlayPrimitives(overlay, false, false, true).worldSegments.size() == 7);
    CHECK(editor::compileOverlayPrimitives(overlay, false, true, false).worldSegments.size() == 2);
    overlay.showLeak = false;
    CHECK(editor::compileOverlayPrimitives(overlay, false, true, true).worldPoints.empty());

    overlay.showLeak = true;
    const auto live = editor::compileOverlayPrimitives(overlay, false, true, false);
    const auto stale = editor::compileOverlayPrimitives(overlay, true, true, false);
    CHECK(live.worldSegments[0].colour != stale.worldSegments[0].colour);
    CHECK(stale.worldSegments[0].colour.r == stale.worldSegments[0].colour.g); // grey
    CHECK(overlay.leakStart() == DVec3(0, 0, 0));
}

TEST_CASE("compile overlay loads from a work directory and treats missing files as empty") {
    const auto loaded = editor::loadCompileOverlay(APEX_COMPILE_LOG_DIR, "leak", 7);
    CHECK(loaded.revision == 7);
    CHECK(loaded.leak.size() == 3);
    CHECK(loaded.portals.portals.empty()); // a leaking compile writes no .prt
    const auto clean = editor::loadCompileOverlay(APEX_COMPILE_LOG_DIR, "baseline", 8);
    CHECK(clean.leak.empty());
    CHECK(clean.portals.portals.size() == 75);
    CHECK_FALSE(clean.leakStart());
}
