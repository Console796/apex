#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/visgroups.hpp"
#include "editor/visibility.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace editor_test;

namespace {
std::string bytes(const Document& doc) {
    const auto result = doc.write();
    REQUIRE(result);
    return *result;
}
const kv1::Node* editorOf(const Document& doc, NodeId node) {
    const auto* n = doc.get(*doc.handle(node));
    for (const auto& b : n->block->children()) if (ciEquals(b.key(), "editor")) return &b;
    for (const auto child : n->children) {
        const auto* b = doc.get(*doc.handle(child))->block.get();
        if (ciEquals(b->key(), "editor")) return b;
    }
    return nullptr;
}
std::vector<std::pair<std::string, std::string>> entries(const Document& doc, NodeId node) {
    std::vector<std::pair<std::string, std::string>> result;
    if (const auto* b = editorOf(doc, node)) for (const auto& v : b->children())
        if (v.kind() == kv1::NodeKind::Value) result.emplace_back(v.key(), v.decodedValue().value());
    return result;
}
void flag(const Document& doc, NodeId node, std::string_view key, std::string_view expected) {
    const auto* b = editorOf(doc, node);
    REQUIRE(b);
    REQUIRE(b->find(key));
    CHECK(b->find(key)->decodedValue() == expected);
}
void checkTree(const VisgroupTree& a, const VisgroupTree& b) {
    REQUIRE(a.nodes.size() == b.nodes.size());
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        CHECK(a.nodes[i].vmfId == b.nodes[i].vmfId);
        CHECK(a.nodes[i].name == b.nodes[i].name);
        CHECK(a.nodes[i].color == b.nodes[i].color);
        CHECK(a.nodes[i].children.size() == b.nodes[i].children.size());
        CHECK((a.nodes[i].parent == 0) == (b.nodes[i].parent == 0));
    }
}
}

TEST_CASE("Visgroups create rename nest delete round trip and undo exactly") {
    for (const auto& text : {std::string("world { id 1 }"), [] {
        std::ifstream in(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf");
        return std::string(std::istreambuf_iterator<char>(in), {});
    }()}) {
        Document doc(parsed(text));
        EditorSession session;
        session.attach(doc);
        const auto before = bytes(doc);
        const auto history = doc.undoCount();
        const auto a = session.createVisgroup("A");
        REQUIRE(a);
        const auto b = session.createVisgroup("B", *a);
        REQUIRE(b);
        CHECK(*a != *b);
        const auto tree = buildVisgroupTree(doc);
        CHECK(tree.find(*b)->parent == tree.find(*a)->node);
        const auto unchanged = bytes(doc);
        CHECK_FALSE(session.moveVisgroup(*a, *b));
        CHECK(bytes(doc) == unchanged);
        REQUIRE(session.renameVisgroup(*b, "renamed"));
        REQUIRE(session.moveVisgroup(*b, {}, 0));
        CHECK(buildVisgroupTree(doc).find(*b)->parent == 0);
        REQUIRE(session.moveVisgroup(*b, *a, 0));
        Document roundTrip(parsed(bytes(doc)));
        checkTree(buildVisgroupTree(doc), buildVisgroupTree(roundTrip));
        REQUIRE(session.deleteVisgroup(*a));
        CHECK_FALSE(buildVisgroupTree(doc).find(*a));
        CHECK_FALSE(buildVisgroupTree(doc).find(*b));
        REQUIRE(doc.jumpTo(history));
        CHECK(bytes(doc) == before);
    }
}

TEST_CASE("Visgroup membership preserves duplicate keys and selection targets") {
    Document doc(parsed(R"(world { id 1 group { id 10 } solid { id 2 editor {
        color "1 2 3" visgroupid 71 comments keep VisgroupID 72 visgroupid 71 groupid 10 } } }
        entity { id 3 classname func_detail solid { id 4 } editor { comments entity } })"));
    EditorSession session;
    session.attach(doc);
    const auto vg = session.createVisgroup("new").value();
    NodeId solid = 0, brushSolid = 0;
    for (const auto h : nodes(doc)) {
        if (vmfId(doc, id(doc, h)) == 2) solid = id(doc, h);
        if (vmfId(doc, id(doc, h)) == 4) brushSolid = id(doc, h);
    }
    const auto before = entries(doc, solid);
    session.selection().select(*session.groups().group(10));
    REQUIRE(session.addSelectionToVisgroup(vg));
    const auto added = bytes(doc);
    REQUIRE(session.addSelectionToVisgroup(vg));
    CHECK(bytes(doc) == added);
    CHECK(buildVisgroupTree(doc).members(vg) == std::vector<NodeId>{solid});
    REQUIRE(session.removeSelectionFromVisgroup(vg));
    CHECK(entries(doc, solid) == before);
    session.selection().setMode(SelectionMode::Solids);
    session.selection().select(brushSolid);
    REQUIRE(session.addSelectionToVisgroup(vg));
    CHECK(buildVisgroupTree(doc).members(vg) == std::vector<NodeId>{brushSolid});
    const auto count = doc.undoCount();
    REQUIRE(session.selectVisgroupMembers(vg));
    CHECK(session.selection().contains(brushSolid));
    CHECK(doc.undoCount() == count);
    const auto original = bytes(doc);
    CHECK_FALSE(session.renameVisgroup(99999, "bad"));
    CHECK_FALSE(session.moveVisgroup(vg, 99999));
    CHECK_FALSE(session.deleteVisgroup(99999));
    CHECK_FALSE(session.addSelectionToVisgroup(99999));
    CHECK_FALSE(session.removeSelectionFromVisgroup(99999));
    CHECK_FALSE(session.setVisgroupShown(99999, false));
    CHECK_FALSE(session.selectVisgroupMembers(99999));
    CHECK_FALSE(session.setAutoVisgroupShown("bad", false));
    CHECK_FALSE(session.selectAutoMembers("bad"));
    CHECK(bytes(doc) == original);
}

TEST_CASE("User visgroups preserve overlapping and plain hides with exact undo redo") {
    Document doc(parsed("world { id 1 } entity { id 2 classname func_detail }"));
    const auto brush = *doc.handle(doc.roots().back());
    const auto box = id(doc, addBox(doc, {0, 0, 0}, {8, 8, 8}, brush));
    const auto entityId = id(doc, brush);
    const auto plain = id(doc, addBox(doc, {16, 0, 0}, {24, 8, 8}, worldHandle(doc)));
    EditorSession s;
    s.attach(doc);
    const auto a = s.createVisgroup("A").value();
    const auto b = s.createVisgroup("B").value();
    s.selection().select(plain);
    REQUIRE(s.hideSelection());
    s.selection().add(entityId);
    s.selection().add(plain);
    REQUIRE(s.addSelectionToVisgroup(a));
    REQUIRE(s.addSelectionToVisgroup(b));
    const auto before = bytes(doc);
    REQUIRE(s.setVisgroupShown(a, false));
    CHECK(s.selection().ordered().empty());
    CHECK(isHiddenByWrapper(doc, entityId));
    CHECK(isHiddenByWrapper(doc, box));
    flag(doc, entityId, "visgroupshown", "0");
    flag(doc, box, "visgroupshown", "0");
    const auto hidden = bytes(doc);
    REQUIRE(s.perform("edit.undo"));
    CHECK(bytes(doc) == before);
    REQUIRE(s.perform("edit.redo"));
    CHECK(bytes(doc) == hidden);
    REQUIRE(s.setVisgroupShown(b, false));
    REQUIRE(s.setVisgroupShown(a, true));
    CHECK_FALSE(isVisible(doc, entityId));
    REQUIRE(s.setVisgroupShown(b, true));
    CHECK(isVisible(doc, entityId));
    CHECK(isVisible(doc, box));
    CHECK_FALSE(isVisible(doc, plain));
    flag(doc, entityId, "visgroupshown", "1");
    REQUIRE(s.perform("edit.undo"));
    CHECK_FALSE(isVisible(doc, entityId));
    REQUIRE(s.perform("edit.redo"));
    REQUIRE(s.setVisgroupShown(a, false));
    REQUIRE(s.deleteVisgroup(a));
    CHECK(isVisible(doc, entityId));
    CHECK(isVisible(doc, box));
    CHECK_FALSE(isVisible(doc, plain));
    CHECK(buildVisgroupTree(doc).visgroupsOf(entityId) == std::vector<int>{b});
}

TEST_CASE("Auto visgroups have stable predicates and independent hide reasons") {
    Document doc(parsed(R"(world { id 1 solid { id 2 side { id 3 material tools/toolsnodraw }
        side { id 4 material TOOLS/toolsnodraw } } }
        entity { id 5 classname light_spot } entity { id 6 classname info_target }
        entity { id 7 classname trigger_once solid { id 8 side { id 9 material brick/wall } } })"));
    EditorSession s;
    s.attach(doc);
    REQUIRE(autoVisgroups().size() == 7);
    CHECK(autoVisgroups()[0].id == "tool-brushes");
    const auto tool = autoMembers(doc, "tool-brushes");
    const auto light = autoMembers(doc, "lights");
    REQUIRE(tool.size() == 1);
    REQUIRE(light.size() == 1);
    CHECK(autoMembers(doc, "nodraw") == tool);
    CHECK(autoMembers(doc, "triggers").size() == 1);
    CHECK(autoMembers(doc, "point-entities").size() == 2);
    CHECK(autoMembers(doc, "brush-entities").size() == 1);
    const auto original = bytes(doc);
    REQUIRE(s.setAutoVisgroupShown("tool-brushes", false));
    REQUIRE(s.setAutoVisgroupShown("lights", false));
    for (const auto node : {tool[0], light[0]}) {
        CHECK(isHiddenByWrapper(doc, node));
        flag(doc, node, "visgroupautoshown", "0");
    }
    REQUIRE(s.setAutoVisgroupShown("nodraw", false));
    REQUIRE(s.setAutoVisgroupShown("tool-brushes", true));
    CHECK_FALSE(isVisible(doc, tool[0]));
    REQUIRE(s.setAutoVisgroupShown("nodraw", true));
    REQUIRE(s.setAutoVisgroupShown("lights", true));
    CHECK(isVisible(doc, tool[0]));
    CHECK(isVisible(doc, light[0]));
    flag(doc, tool[0], "visgroupautoshown", "1");
    REQUIRE(s.selectAutoMembers("lights"));
    CHECK(s.selection().contains(light[0]));
    REQUIRE(doc.jumpTo(0));
    CHECK(bytes(doc) == original);
}

TEST_CASE("Visgroups corpus membership edits preserve every other editor entry") {
    namespace fs = std::filesystem;
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install || !fs::is_directory(fs::path(install) / "sdk_content/maps")) {
        MESSAGE("P2CE corpus unavailable; skipping visgroup corpus checks");
        return;
    }
    for (const auto& entry : fs::recursive_directory_iterator(fs::path(install) / "sdk_content/maps")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".vmf") continue;
        CAPTURE(entry.path().string());
        std::ifstream stream(entry.path(), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(stream), {}};
        auto file = vmf::parse(text);
        if (!file) file = vmf::parse(text, {kv1::EscapeMode::Legacy});
        REQUIRE(file);
        Document doc(*file);
        const auto tree = buildVisgroupTree(doc);
        for (const auto& [group, members] : tree.membership) {
            CHECK((tree.find(group) != nullptr || tree.dangling.contains(group)));
            bool shown = true;
            for (const auto member : members) {
                const auto* b = editorOf(doc, member);
                REQUIRE(b);
                if (const auto* v = b->find("visgroupshown"); v && v->decodedValue() == "0") shown = false;
            }
            CHECK(tree.shown(group) == shown);
        }
        EditorSession s;
        s.attach(doc);
        const auto initial = bytes(doc);
        const auto vg = s.createVisgroup("corpus check").value();
        std::map<NodeId, std::vector<std::pair<std::string, std::string>>> originals;
        s.selection().setMode(SelectionMode::Objects);
        for (const auto h : nodes(doc)) {
            if (!std::holds_alternative<Handle<Solid>>(h) && !std::holds_alternative<Handle<Entity>>(h)) continue;
            if (ciEquals(doc.get(h)->block->key(), "world")) continue;
            const auto node = id(doc, h);
            const auto* editor = editorOf(doc, node);
            if (!editor || editor->findAll("visgroupid").size() < 2) continue;
            originals[node] = entries(doc, node);
            s.selection().add(node);
        }
        REQUIRE(s.addSelectionToVisgroup(vg));
        REQUIRE(s.removeSelectionFromVisgroup(vg));
        for (const auto& [node, original] : originals) CHECK(entries(doc, node) == original);
        std::map<int, std::vector<NodeId>> removals;
        for (const auto& [node, original] : originals) {
            (void)original;
            for (const auto group : tree.visgroupsOf(node))
                if (tree.find(group)) removals[group].push_back(node);
        }
        for (const auto& [group, members] : removals) {
            s.selection().clear();
            for (const auto member : members) s.selection().add(member);
            REQUIRE(s.removeSelectionFromVisgroup(group));
            for (const auto member : members) {
                auto expected = originals.at(member);
                std::erase_if(expected, [&](const auto& pair) {
                    return ciEquals(pair.first, "visgroupid") && pair.second == std::to_string(group);
                });
                CHECK(entries(doc, member) == expected);
            }
            REQUIRE(doc.undo());
        }
        REQUIRE(doc.jumpTo(0));
        CHECK(bytes(doc) == initial);
    }
}

TEST_CASE("Visgroup visibility survives history branching eviction and unhide all") {
    Document doc(parsed("world { id 1 solid { id 2 } }"));
    EditorSession s;
    s.attach(doc);
    const auto a = s.createVisgroup("A").value();
    const auto b = s.createVisgroup("B", a).value();
    NodeId solid = 0;
    for (const auto h : nodes(doc)) if (std::holds_alternative<Handle<Solid>>(h)) solid = id(doc, h);
    s.selection().select(solid);
    REQUIRE(s.addSelectionToVisgroup(a));
    REQUIRE(s.addSelectionToVisgroup(b));
    REQUIRE(s.setVisgroupShown(a, false));
    REQUIRE(s.setVisgroupShown(b, false));
    REQUIRE(s.perform("edit.undo"));
    REQUIRE(s.renameVisgroup(b, "branch"));
    REQUIRE(s.setVisgroupShown(a, true));
    CHECK(isVisible(doc, solid));
    REQUIRE(s.setVisgroupShown(b, false));
    REQUIRE(s.unhideAll());
    REQUIRE(s.setVisgroupShown(a, false));
    REQUIRE(s.setVisgroupShown(a, true));
    CHECK(isVisible(doc, solid));
    REQUIRE(s.perform("edit.undo"));
    CHECK_FALSE(isVisible(doc, solid));
    REQUIRE(s.perform("edit.redo"));
    CHECK(isVisible(doc, solid));
    REQUIRE(s.setVisgroupShown(b, false));
    const auto beforeDelete = bytes(doc);
    REQUIRE(s.deleteVisgroup(a));
    CHECK(isVisible(doc, solid));
    CHECK(buildVisgroupTree(doc).visgroupsOf(solid).empty());
    REQUIRE(s.perform("edit.undo"));
    CHECK(bytes(doc) == beforeDelete);
    doc.setUndoCap(1);
    CHECK(doc.undoCount() == 0);
    REQUIRE(s.setVisgroupShown(b, true));
    CHECK(isVisible(doc, solid));
    REQUIRE(s.setVisgroupShown(a, false));
    CHECK_FALSE(isVisible(doc, solid));
    doc.setUndoCap(1024 * 1024);
    REQUIRE(s.setVisgroupShown(a, true));
    CHECK(isVisible(doc, solid));
    REQUIRE(s.perform("edit.undo"));
    CHECK_FALSE(isVisible(doc, solid));
}

TEST_CASE("User and auto visgroups preserve each other's visibility flags") {
    Document doc(parsed(R"(world { id 1 } entity { id 2 classname light })"));
    EditorSession s;
    s.attach(doc);
    const auto light = autoMembers(doc, "lights").front();
    const auto vg = s.createVisgroup("light").value();
    s.selection().select(light);
    REQUIRE(s.addSelectionToVisgroup(vg));
    REQUIRE(s.setVisgroupShown(vg, false));
    REQUIRE(s.setAutoVisgroupShown("lights", false));
    REQUIRE(s.setVisgroupShown(vg, true));
    CHECK_FALSE(isVisible(doc, light));
    flag(doc, light, "visgroupshown", "1");
    flag(doc, light, "visgroupautoshown", "0");
    REQUIRE(s.setAutoVisgroupShown("lights", true));
    CHECK(isVisible(doc, light));
    const auto shown = bytes(doc);
    REQUIRE(s.perform("edit.undo"));
    CHECK_FALSE(isVisible(doc, light));
    REQUIRE(s.perform("edit.redo"));
    CHECK(bytes(doc) == shown);
}
