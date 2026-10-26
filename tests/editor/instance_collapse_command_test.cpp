#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/instance_templates.hpp"
#include "corpus.hpp"

#include <cstdlib>
#include <filesystem>

using namespace editor_test;

namespace {
Document instanceMap() {
    return Document(parsed(R"vmf(versioninfo { "editorversion" "400" }
visgroups { visgroup { "name" "test" "visgroupid" "20" } }
world { "id" "1" "classname" "worldspawn" }
group { "id" "10" }
entity { "id" "2" "classname" "func_instance" "targetname" "placed" "file" "instance_command_template.vmf" "origin" "32 0 0" "angles" "0 0 0" editor { "groupid" "10" "visgroupid" "20" } }
entity { "id" "3" "classname" "logic_relay" "targetname" "external" connections { "OnTrigger" "placed,instance:inside;Trigger,hello,0,-1" } }
)vmf"));
}
std::size_t namedClassCount(const Document& doc, std::string_view classname) {
    std::size_t count = 0;
    for (const auto h : nodes(doc)) {
        const auto exported = doc.exportSubtree(doc.get(h)->id);
        const auto* block = &exported;
        const auto* value = block ? block->find("classname") : nullptr;
        if (value && ciEquals(value->decodedValue().value_or(""), classname)) ++count;
    }
    return count;
}
}

TEST_CASE("[editor][instance] collapse is one undoable paste transaction") {
    Document doc = instanceMap();
    EditorSession session;
    session.attach(doc);
    session.setConnectionContext(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf" / "instances" / "map.vmf");
    const auto before = doc.write().value();
    NodeId instance = 0;
    for (const auto h : nodes(doc)) {
        const auto* c = doc.get(h)->block->find("classname");
        if (c && c->decodedValue().value_or("") == "func_instance") instance = doc.get(h)->id;
    }
    REQUIRE(instance != 0);
    const std::array<NodeId, 1> selected{instance};
    auto result = session.collapseInstances(selected);
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->front().collapsed);
    CHECK(namedClassCount(doc, "func_instance") == 0);
    CHECK(namedClassCount(doc, "logic_relay") == 2);
    NodeId collapsedEntity = 0;
    for (const auto h : nodes(doc)) {
        const auto block = doc.exportSubtree(doc.get(h)->id);
        const auto* cls = block.find("classname");
        if (!cls || cls->decodedValue().value_or("") != "logic_relay") continue;
        if (!block.find("targetname") || block.find("targetname")->decodedValue().value_or("") != "placed-inside") continue;
        collapsedEntity = doc.get(h)->id;
        CHECK(block.find("id")->asInt().value() != 1);
        CHECK(block.find("id")->asInt().value() != 2);
        REQUIRE(block.find("editor") != nullptr);
        CHECK(block.find("editor")->find("groupid")->asInt().value() == 10);
        CHECK(block.find("editor")->find("visgroupid")->asInt().value() == 20);
    }
    CHECK(collapsedEntity != instance);
    bool rewroteExternal = false;
    for (const auto h : nodes(doc)) {
        const auto exported = doc.exportSubtree(doc.get(h)->id);
        const auto* block = &exported;
        if (!block || block->find("targetname") == nullptr ||
            block->find("targetname")->decodedValue().value_or("") != "external") continue;
        const auto* connections = block->find("connections");
        REQUIRE(connections);
        const auto* row = connections->find("OnTrigger");
        REQUIRE(row);
        CHECK(row->decodedValue().value_or("").find("instance:") == std::string::npos);
        CHECK(row->decodedValue().value_or("").find("placed-inside,Trigger") != std::string::npos);
        rewroteExternal = true;
    }
    CHECK(rewroteExternal);
    CHECK(session.selection().ordered().size() == 1);
    const auto collapsed = doc.write().value();
    CHECK(collapsed != before);
    REQUIRE(session.perform("edit.undo"));
    CHECK(doc.write().value() == before);
    REQUIRE(session.perform("edit.redo"));
    CHECK(doc.write().value() == collapsed);
    auto reloaded = vmf::parse(collapsed);
    REQUIRE(reloaded);
    Document roundtrip(std::move(*reloaded));
    CHECK(namedClassCount(roundtrip, "func_instance") == 0);
}

TEST_CASE("[editor][instance] collapse all recursively removes nested instances") {
    Document doc(parsed(R"vmf(versioninfo { "editorversion" "400" }
world { "id" "1" "classname" "worldspawn" }
entity { "id" "2" "classname" "func_instance" "targetname" "first" "file" "instance_command_nested.vmf" "origin" "0 0 0" "angles" "0 0 0" }
entity { "id" "3" "classname" "func_instance" "targetname" "second" "file" "instance_command_nested.vmf" "origin" "64 0 0" "angles" "0 0 0" }
)vmf"));
    EditorSession session;
    session.attach(doc);
    session.setConnectionContext(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf" / "instances" / "map.vmf");
    auto result = session.collapseAllInstances();
    REQUIRE(result);
    CHECK(result->size() == 2);
    CHECK(std::ranges::all_of(*result, [](const auto& item) { return item.collapsed; }));
    CHECK(namedClassCount(doc, "func_instance") == 0);
    CHECK(namedClassCount(doc, "logic_relay") == 2);
    CHECK(session.selection().ordered().empty());
    std::set<std::string> targetnames;
    for (const auto h : nodes(doc)) {
        const auto block = doc.exportSubtree(doc.get(h)->id);
        const auto* name = block.find("targetname");
        if (name && block.find("classname") && block.find("classname")->decodedValue().value_or("") == "logic_relay")
            CHECK(targetnames.insert(name->decodedValue().value_or("")).second);
    }
}

TEST_CASE("[editor][instance] collapse reports unresolved instances and leaves them in place") {
    Document doc = instanceMap();
    EditorSession session;
    session.attach(doc);
    session.setConnectionContext("/path/that/does/not/exist/map.vmf");
    const auto before = doc.write().value();
    auto result = session.collapseAllInstances();
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK_FALSE(result->front().collapsed);
    CHECK_FALSE(result->front().reason.empty());
    CHECK(doc.write().value() == before);
}

TEST_CASE("[editor][instance] collapse all skips unresolved and cyclic instances independently") {
    Document doc(parsed(R"vmf(versioninfo { "editorversion" "400" }
world { "id" "1" "classname" "worldspawn" }
entity { "id" "2" "classname" "func_instance" "targetname" "good" "file" "instance_command_template.vmf" }
entity { "id" "3" "classname" "func_instance" "targetname" "missing" "file" "missing_template.vmf" }
entity { "id" "4" "classname" "func_instance" "targetname" "cycle" "file" "instance_command_cycle_a.vmf" }
)vmf"));
    EditorSession session;
    session.attach(doc);
    session.setConnectionContext(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf" / "instances" / "map.vmf");
    const auto result = session.collapseAllInstances();
    REQUIRE(result);
    REQUIRE(result->size() == 3);
    CHECK((*result)[0].collapsed);
    CHECK_FALSE((*result)[1].collapsed);
    CHECK_FALSE((*result)[1].reason.empty());
    CHECK_FALSE((*result)[2].collapsed);
    CHECK((*result)[2].reason.find("cycle") != std::string::npos);
    CHECK(namedClassCount(doc, "func_instance") == 2);
    CHECK(namedClassCount(doc, "logic_relay") == 1);
}

TEST_CASE("[editor][instance][corpus] collapse all removes resolvable instances") {
    const auto paths = corpusPaths();
    if (paths.empty()) { MESSAGE("APEX_P2CE_DIR is unset; skipping instance collapse corpus"); return; }
    std::size_t maps = 0, withInstances = 0, collapsed = 0;
    for (const auto& path : paths) {
        CAPTURE(path.string());
        Document doc(readCorpus(path));
        std::set<NodeId> expected;
        for (const auto h : nodes(doc)) {
            const auto* block = doc.get(h)->block.get();
            const auto* cls = block ? block->find("classname") : nullptr;
            if (!cls || !ciEquals(cls->decodedValue().value_or(""), "func_instance")) continue;
            const auto* file = block->find("file");
            if (file && resolveInstanceFile(path, file->decodedValue().value_or(""))) expected.insert(doc.get(h)->id);
        }
        if (expected.empty()) continue;
        ++maps;
        ++withInstances;
        EditorSession session;
        session.attach(doc);
        session.setConnectionContext(path);
        const auto result = session.collapseAllInstances();
        REQUIRE(result);
        for (const auto& item : *result) {
            if (expected.contains(item.instance)) {
                CAPTURE(item.instance);
                CHECK_MESSAGE(item.collapsed, item.reason);
                if (item.collapsed) ++collapsed;
            }
        }
        for (const auto h : nodes(doc)) {
            const auto* block = doc.get(h)->block.get();
            const auto* cls = block ? block->find("classname") : nullptr;
            if (!cls || !ciEquals(cls->decodedValue().value_or(""), "func_instance")) continue;
            const auto* file = block->find("file");
            const bool stillResolvable = file && resolveInstanceFile(path, file->decodedValue().value_or("")).has_value();
            CHECK_FALSE(stillResolvable);
        }
    }
    MESSAGE("instance corpus maps=" << maps << " maps_with_instances=" << withInstances << " collapsed=" << collapsed);
}
