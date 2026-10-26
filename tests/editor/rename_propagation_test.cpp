#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/rename_propagation.hpp"
#include "core/strings.hpp"

#include <cstdlib>
#include <fstream>

using namespace editor_test;
namespace {
std::string bytes(const Document& doc) {
    const auto result = doc.write();
    REQUIRE(result);
    return *result;
}
assets::EntityClassTable renameClasses() {
    assets::EntityClassTable classes;
    auto& button = classes.classes["func_button"];
    assets::EntityProperty parent;
    parent.name = "parentname";
    parent.type = "target_destination";
    button.properties.push_back(parent);
    assets::EntityProperty target;
    target.name = "target";
    target.type = "target_name_or_class";
    button.properties.push_back(target);
    button.helpers.push_back({"line", "255 255 255, targetname, line_target"});
    return classes;
}
NodeId named(const Document& doc, std::string_view name) {
    for (const auto h : nodes(doc)) {
        if (!std::holds_alternative<Handle<Entity>>(h)) continue;
        const auto* field = doc.get(h)->block->find("targetname");
        if (field && ciEquals(field->decodedValue().value_or(""), name)) return doc.get(h)->id;
    }
    return 0;
}
std::string connectionValue(const Document& doc, NodeId entity, std::size_t index) {
    const auto* node = doc.get(*doc.handle(entity));
    for (const auto childId : node->children) {
        const auto* child = doc.get(*doc.handle(childId));
        if (!ciEquals(child->block->key(), "connections")) continue;
        return child->block->children()[index].decodedValue().value_or("");
    }
    return {};
}
}

TEST_CASE("Rename plan lists exact connection and typed helper references in document order") {
    const std::string esc = "Door_A\x1b" "Open\x1b\x1b" "0\x1b-1";
    const std::string comma = "door_a,Close,,1,-1";
    Document doc(parsed("entity { id 1 classname func_button targetname button connections { "
        "OnPressed \"Door_A\x1b" "Open\x1b\x1b" "0\x1b" "-1\" "
        "OnPressed \"door_a,Close,,1,-1\" "
        "OnPressed \"*Door_A,Open,,0,-1\" "
        "OnPressed \"!self,Open,,0,-1\" "
        "OnPressed \"instance:Door_A;Trigger,Open,,0,-1\" "
        "OnPressed \"Door_A,broken\" } parentname Door_A target Door_A line_target Door_A custom Door_A } "
        "entity { id 2 classname info_target targetname Door_A }"));
    const auto target = named(doc, "Door_A");
    const auto plan = planRename(doc, renameClasses(), target, "door_b");
    REQUIRE(plan.changes.size() == 5);
    CHECK(plan.oldName == "Door_A");
    CHECK(plan.newName == "door_b");
    CHECK(plan.changes[0].kind == RenameChange::Kind::Connection);
    CHECK(plan.changes[0].where == "OnPressed #1");
    CHECK(plan.changes[0].before == "Door_A");
    CHECK(plan.changes[1].kind == RenameChange::Kind::Connection);
    CHECK(plan.changes[1].where == "OnPressed #2");
    CHECK(plan.changes[1].before == "door_a");
    CHECK(plan.changes[2].where == "parentname");
    CHECK(plan.changes[3].where == "target");
    CHECK(plan.changes[4].where == "line_target");
    const auto lines = describe(plan);
    REQUIRE(lines.size() == 5);
    CHECK(lines.front() == "func_button (button) OnPressed #1: Door_A -> door_b");
    CHECK_FALSE(std::ranges::any_of(plan.changes, [](const auto& change) { return change.where == "custom"; }));
    CHECK(esc.find('\x1b') != std::string::npos);
    CHECK(comma.find(',') != std::string::npos);
}

TEST_CASE("Propagating rename is one undo step and preserves connection separators and unrelated bytes") {
    const std::string source = "entity { id 1 classname func_button targetname button connections { "
        "OnPressed \"Door_A\x1b" "Open\x1b\x1b" "0\x1b" "-1\" "
        "OnPressed \"door_a,Close,,1,-1\" } parentname Door_A target Door_A line_target Door_A custom Door_A } "
        "entity { id 2 classname info_target targetname Door_A }";
    Document doc(parsed(source));
    const auto target = named(doc, "Door_A");
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(renameClasses());
    const auto plan = session.planEntityRename(target, "door_b");
    REQUIRE(plan.changes.size() == 5);
    const auto before = bytes(doc);
    const auto history = doc.undoCount();
    REQUIRE(session.renameEntity(target, "door_b", true));
    CHECK(doc.undoCount() == history + 1);
    CHECK(bytes(doc).find("\"Door_A\x1b") == std::string::npos);
    CHECK(bytes(doc).find("\"door_b\x1b" "Open\x1b\x1b" "0\x1b" "-1\"") != std::string::npos);
    CHECK(bytes(doc).find("\"door_b,Close,,1,-1\"") != std::string::npos);
    CHECK(bytes(doc).find("custom Door_A") != std::string::npos);
    REQUIRE(doc.undo());
    CHECK(bytes(doc) == before);
}

TEST_CASE("Rename without propagation and empty or unchanged plans preserve references") {
    Document doc(parsed("entity { id 1 classname func_button targetname door_a connections { "
                        "OnPressed \"door_a,Open,,0,-1\" } }"));
    const auto target = named(doc, "door_a");
    const auto classes = renameClasses();
    CHECK(planRename(doc, classes, target, "").changes.empty());
    CHECK(planRename(doc, classes, target, "door_a").changes.empty());
    EditorSession session;
    session.attach(doc);
    session.setEntityClasses(classes);
    const auto original = bytes(doc);
    REQUIRE(session.renameEntity(target, "door_b", false));
    CHECK(bytes(doc).find("\"targetname\" \"door_b\"") != std::string::npos);
    CHECK(connectionValue(doc, named(doc, "door_b"), 0) == "door_a,Open,,0,-1");
    REQUIRE(doc.undo());
    CHECK(bytes(doc) == original);
}

TEST_CASE("Rename plan corpus scan keeps every reported before value tied to its targetname") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("APEX_P2CE_DIR unset; skipping rename propagation corpus"); return; }
    const auto root = std::filesystem::path(install) / "sdk_content/maps";
    REQUIRE(std::filesystem::is_directory(root));
    std::vector<std::filesystem::path> maps;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.is_regular_file() && ciEquals(entry.path().extension().string(), ".vmf")) maps.push_back(entry.path());
    std::ranges::sort(maps);
    REQUIRE_FALSE(maps.empty());
    const assets::EntityClassTable classes;
    std::size_t entities = 0, changes = 0;
    for (const auto& path : maps) {
        CAPTURE(path.string());
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        const std::string text{std::istreambuf_iterator<char>(stream), {}};
        auto file = vmf::parse(text);
        if (!file) file = vmf::parse(text, {kv1::EscapeMode::Legacy});
        REQUIRE(file);
        Document doc(*file);
        for (const auto h : nodes(doc)) {
            if (!std::holds_alternative<Handle<Entity>>(h)) continue;
            const auto* field = doc.get(h)->block->find("targetname");
            const auto old = field ? field->decodedValue().value_or("") : "";
            if (old.empty()) continue;
            ++entities;
            const auto plan = planRename(doc, classes, doc.get(h)->id, "zz_renamed");
            for (const auto& change : plan.changes) {
                CHECK(ciEquals(change.before, old));
                ++changes;
            }
        }
    }
    MESSAGE("Rename corpus: ", maps.size(), " maps, ", entities, " named entities, ", changes, " references");
    CHECK(entities > 0);
}
