#include "fixtures.hpp"
#include "../assets/fixture.hpp"
#include "assets/fgd.hpp"
#include "editor/editor_session.hpp"

#include <algorithm>
#include <cstdlib>

using namespace editor_test;

namespace {
assets::EntityClassTable classes() {
    GameTree tree;
    tree.put("test.fgd", R"(@PointClass size(-4 -4 0, 4 4 8) = test [
        text(string) : "Text" : "hello"
        count(integer) : "Count" : 3
        rate(float) : "Rate" : "1.5"
        mode(choices) : "Mode" : 1 = [ 1 : "One" 2 : "Two" ]
        spawnflags(flags) = [ 1 : "One" : 1 4 : "Four" : 0 ]
        color(color255) : "Color" : "255 255 255"
        origin(origin) : "Origin" : "0 0 0"
        angles(angle) : "Angles" : "0 0 0"
        target(target_destination) : "Target"
        source(target_source) : "Source"
        locked(string) readonly : "Locked" : "keep"
    ]
    )");
    auto result = assets::loadEntityClasses(tree.root / "test.fgd");
    REQUIRE_MESSAGE(result, (result ? "" : result.error().what()));
    REQUIRE(result->find("test"));
    REQUIRE(result->find("test")->properties.size() == 11);
    return std::move(*result);
}
std::size_t row(const InspectorModel& model, std::string_view key, std::size_t occurrence = 0) {
    for (std::size_t i = 0; i < model.rows().size(); ++i)
        if (model.rows()[i].key == key && model.rows()[i].occurrence == occurrence) return i;
    FAIL("missing inspector row: ", key);
    return 0;
}
std::string text(const Document& doc, NodeId id, std::string_view key) {
    return doc.get(*doc.handle(id))->block->find(key)->decodedValue().value();
}
std::map<std::string, std::string> values(const Document& doc, NodeId id) {
    std::map<std::string, std::string> result;
    for (const auto& child : doc.get(*doc.handle(id))->block->children())
        if (child.kind() == kv1::NodeKind::Value) result[std::string(child.key())] = child.decodedValue().value();
    return result;
}
} // namespace

TEST_CASE("inspector edits each property type without touching any other key") {
    const auto table = classes();
    Document doc(parsed(R"(entity { "id" "1" "classname" "test" "unknown" "preserved" })"));
    const auto entity = doc.roots().front();
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(table);
    session.selectNode(entity);
    for (const auto& [key, input] : std::vector<std::pair<std::string, std::string>>{
             {"text", "edited"}, {"count", "42"}, {"rate", "2.75"}, {"mode", "2"},
             {"spawnflags", "5"}, {"color", "1 20 255"}, {"origin", "10 20 30"},
             {"angles", "0 90 0"}, {"target", "door"}, {"source", "relay"}}) {
        INFO(key);
        auto expected = values(doc, entity);
        expected[key] = input;
        const auto model = session.inspectorModel();
        const auto index = row(model, key);
        const auto history = doc.undoCount();
        if (key == "mode") REQUIRE(session.setChoice(index, input));
        else if (key == "spawnflags") REQUIRE(session.toggleFlag(index, 4, true));
        else REQUIRE(session.editProperty(index, input));
        CHECK(values(doc, entity) == expected);
        CHECK(doc.undoCount() == history + 1);
    }
    REQUIRE(session.gizmoContext());
    checkPoint(session.picks().bounds(entity)->center(), {10, 20, 34});
    CHECK_FALSE(session.editProperty(row(session.inspectorModel(), "locked"), "no"));
    CHECK_FALSE(session.setChoice(row(session.inspectorModel(), "mode"), "invalid"));
    const auto beforeClass = values(doc, entity);
    REQUIRE(session.setClass("unknown_class"));
    auto expected = beforeClass;
    expected["classname"] = "unknown_class";
    CHECK(values(doc, entity) == expected);
    CHECK(session.inspectorModel().rows()[0].section == InspectorModel::Section::Other);
}

TEST_CASE("inspector mixed multi-selection only writes edited shared fields") {
    const auto table = classes();
    Document doc(parsed(R"(entity { "id" "1" "classname" "test" "text" "a" "spawnflags" "1" "only_first" "x" }
                           entity { "id" "2" "classname" "test" "text" "b" "spawnflags" "8" })"));
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(table);
    session.selection().replace(std::vector<Target>{doc.roots()[0], doc.roots()[1]});
    const auto saved = doc.write().value();
    const auto model = session.inspectorModel();
    CHECK(model.rows()[row(model, "text")].mixed);
    CHECK(model.rows()[row(model, "spawnflags")].mixed);
    CHECK(std::ranges::none_of(model.rows(), [](const auto& r) { return r.key == "only_first"; }));
    CHECK(doc.write().value() == saved);
    REQUIRE(session.editProperty(row(model, "text"), "c", "focus1"));
    REQUIRE(session.editProperty(row(session.inspectorModel(), "text"), "cd", "focus1"));
    CHECK(doc.undoCount() == 1);
    CHECK(text(doc, doc.roots()[0], "spawnflags") == "1");
    CHECK(text(doc, doc.roots()[1], "spawnflags") == "8");
    REQUIRE(session.editProperty(row(session.inspectorModel(), "text"), "cde", "focus2"));
    CHECK(doc.undoCount() == 2);
    REQUIRE(session.toggleFlag(row(session.inspectorModel(), "spawnflags"), 4, true));
    CHECK(text(doc, doc.roots()[0], "spawnflags") == "5");
    CHECK(text(doc, doc.roots()[1], "spawnflags") == "12");
    REQUIRE(session.perform("edit.undo"));
    REQUIRE(session.perform("edit.undo"));
    REQUIRE(session.perform("edit.undo"));
    CHECK(doc.write().value() == saved);
}

TEST_CASE("inspector raw rows preserve file order and edit one duplicate occurrence") {
    Document doc(parsed(R"(entity { "id" "1" "classname" "test" "dup" "a" "Dup" "b" "tail" "z" })"));
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(classes());
    const auto entity = doc.roots().front();
    session.selectNode(entity);
    const auto raw = session.inspectorModel(true);
    std::vector<std::string> keys;
    for (const auto& r : raw.rows()) keys.push_back(r.key);
    CHECK(keys == std::vector<std::string>{"id", "classname", "dup", "Dup", "tail"});
    const auto before = doc.write().value();
    REQUIRE(session.editProperty(row(raw, "Dup", 1), "changed", "", true));
    const auto* block = doc.get(*doc.handle(entity))->block.get();
    CHECK(block->findAll("dup").size() == 2);
    CHECK(block->findAll("dup")[0]->decodedValue() == "a");
    CHECK(block->findAll("dup")[1]->decodedValue() == "changed");
    REQUIRE(session.perform("edit.undo"));
    CHECK(doc.write().value() == before);
    REQUIRE(session.setClass("other"));
    CHECK(doc.get(*doc.handle(entity))->block->findAll("dup").size() == 2);
}

TEST_CASE("inspector target autocomplete collects unique map targetnames") {
    Document doc(parsed(R"(entity { "id" "1" "classname" "test" "targetname" "door" }
                           entity { "id" "2" "classname" "test" "targetname" "relay" }
                           entity { "id" "3" "classname" "test" "targetname" "door" })"));
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(classes());
    session.selectNode(doc.roots()[0]);
    const auto model = session.inspectorModel();
    CHECK(model.autocomplete(row(model, "target")) == std::vector<std::string>{"door", "relay"});
    CHECK(model.autocomplete(row(model, "source")) == model.targetCandidates());
    CHECK(model.autocomplete(row(model, "text")).empty());
}

TEST_CASE("inspector constructs for every P2CE class") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    const auto table = assets::loadEntityClasses(std::filesystem::path(install) / "p2ce/p2ce.fgd");
    REQUIRE_MESSAGE(table, (table ? "" : table.error().what()));
    REQUIRE(table->classes.size() > 100);
    Document doc(parsed());
    auto tx = doc.begin("fixture");
    auto entity = kv1::Node::block("entity");
    entity.append(kv1::Node::value("classname", "test")).value();
    const auto h = tx.create(std::move(entity));
    tx.commit(false);
    Selection selection;
    selection.select(Target{id(doc, h)});
    for (const auto& [key, cls] : table->classes) {
        INFO(key);
        auto change = doc.begin("class");
        change.setKeyValue(h, "classname", cls.name);
        change.commit(false);
        const InspectorModel model(doc, selection, *table);
        CHECK(model.entities().size() == 1);
        CHECK(model.rows().size() >= cls.properties.size());
        for (std::size_t i = 0; i < cls.properties.size(); ++i) CHECK(model.rows()[i].key == cls.properties[i].name);
    }
}
