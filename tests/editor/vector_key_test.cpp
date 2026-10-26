#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/transform.hpp"

using namespace editor_test;

namespace {
assets::EntityClassTable classes() {
    assets::EntityClass cls;
    cls.name = "func_door_rotating";
    for (const auto& [name, type] : std::vector<std::pair<std::string, std::string>>{
             {"axis", "axis"}, {"offset", "vecline"}, {"decoy", "vector"},
             {"empty", "vecline"}, {"garbage", "axis"}, {"missing", "vecline"},
             {"badpoint", "vecline"}, {"emptyaxis", "axis"}, {"missingaxis", "axis"}}) {
        assets::EntityProperty property;
        property.name = name;
        property.type = type;
        cls.properties.push_back(std::move(property));
    }
    assets::EntityClassTable result;
    result.classes.emplace(cls.name, std::move(cls));
    return result;
}
const char* entityText = R"(entity {
    classname func_door_rotating origin "10 20 30"
    axis "11 20 30, 10 22 33" offset "13 24 35" decoy "7 8 9"
    empty "" garbage "1 2 3, garbage" badpoint "1 2 nope" emptyaxis ""
})";
std::string text(const Document& doc, NodeHandle h, std::string_view key) {
    const auto* value = doc.get(h)->block->find(key);
    REQUIRE(value);
    return value->decodedValue().value();
}
DVec3 point(std::string_view value) {
    const auto parsed = kv1::Node::value("", std::string(value)).asVec3();
    REQUIRE(parsed);
    return {(*parsed)[0], (*parsed)[1], (*parsed)[2]};
}
void checkKeys(const Document& doc, NodeHandle h, DVec3 a, DVec3 b, DVec3 offset) {
    const auto axis = text(doc, h, "axis");
    const auto comma = axis.find(',');
    REQUIRE(comma != std::string::npos);
    checkPoint(point(std::string_view(axis).substr(0, comma)), a);
    checkPoint(point(std::string_view(axis).substr(comma + 1)), b);
    checkPoint(point(text(doc, h, "offset")), offset);
    CHECK(text(doc, h, "decoy") == "7 8 9");
    CHECK(text(doc, h, "empty").empty());
    CHECK(text(doc, h, "garbage") == "1 2 3, garbage");
    CHECK(text(doc, h, "badpoint") == "1 2 nope");
    CHECK(text(doc, h, "emptyaxis").empty());
    CHECK_FALSE(doc.get(h)->block->find("missing"));
    CHECK_FALSE(doc.get(h)->block->find("missingaxis"));
}
}

TEST_CASE("transform FGD axis and vecline points on point and brush entities round trip") {
    const auto table = classes();
    const auto transform = Transform::translate({5, -2, 7}) *
                           Transform::rotate({0, 0, 1}, 90, {10, 20, 30});
    for (const bool brush : {false, true}) {
        Document doc(parsed());
        const auto h = addBlock(doc, entityText);
        if (brush) addBox(doc, {8, 18, 28}, {12, 22, 32}, h);
        const std::vector<NodeId> selection{id(doc, h)};
        const auto before = doc.write().value();
        REQUIRE(commitTransform(doc, selection, transform, {true, &table}, "transform"));
        checkKeys(doc, h, {15, 19, 37}, {13, 18, 40}, {11, 21, 42});
        CHECK(text(doc, h, "axis") == "15 19 37, 13 18 40");
        CHECK(text(doc, h, "offset") == "11 21 42");
        REQUIRE(commitTransform(doc, selection, transform.inverse(), {true, &table}, "inverse"));
        checkKeys(doc, h, {11, 20, 30}, {10, 22, 33}, {13, 24, 35});
        REQUIRE(doc.undo());
        REQUIRE(doc.undo());
        CHECK(doc.write().value() == before);
    }
}

TEST_CASE("transform without FGD classes preserves vector key bytes") {
    Document doc(parsed());
    const auto h = addBlock(doc, entityText);
    const std::vector<NodeId> selection{id(doc, h)};
    REQUIRE(commitTransform(doc, selection, Transform::translate({5, 6, 7}), {}, "transform"));
    checkKeys(doc, h, {11, 20, 30}, {10, 22, 33}, {13, 24, 35});
    CHECK(text(doc, h, "axis") == "11 20 30, 10 22 33");
    CHECK(text(doc, h, "offset") == "13 24 35");
}

TEST_CASE("session transform entry points use FGD vector keys and undo restores exact text") {
    Document doc(parsed());
    const auto h = addBlock(doc, entityText);
    auto tx = doc.begin("original formatting");
    tx.setKeyValue(h, "axis", "11.00 20 30, 10 22.0 33");
    tx.commit();
    EditorSession session;
    session.setEntityClasses(classes());
    session.attach(doc);
    session.perform("selection.mode.objects");
    session.selectNode(id(doc, h));
    const auto before = doc.write().value();
    REQUIRE(session.rotateSelection({0, 0, 90}));
    checkKeys(doc, h, {10, 21, 30}, {8, 20, 33}, {6, 23, 35});
    REQUIRE(session.moveSelection({5, -2, 7}, true));
    checkKeys(doc, h, {15, 19, 37}, {13, 18, 40}, {11, 21, 42});
    REQUIRE(session.moveSelection({-5, 2, -7}, true));
    REQUIRE(session.rotateSelection({0, 0, -90}));
    checkKeys(doc, h, {11, 20, 30}, {10, 22, 33}, {13, 24, 35});
    REQUIRE(session.perform("edit.undo"));
    REQUIRE(session.perform("edit.undo"));
    REQUIRE(session.perform("edit.undo"));
    REQUIRE(session.perform("edit.undo"));
    CHECK(doc.write().value() == before);
}

TEST_CASE("session clipboard paste transforms FGD vector keys") {
    Document doc(parsed());
    EditorSession session;
    session.setEntityClasses(classes());
    session.attach(doc);
    PasteOptions options;
    options.inPlace = true;
    options.special = PasteSpecial{};
    options.special->offset = {5, -2, 7};
    REQUIRE(session.pasteText(entityText, options));
    REQUIRE(doc.roots().size() == 1);
    checkKeys(doc, *doc.handle(doc.roots().front()), {16, 18, 37}, {15, 20, 40}, {18, 22, 42});
}
