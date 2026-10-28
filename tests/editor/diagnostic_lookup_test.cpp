#include <doctest/doctest.h>
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
