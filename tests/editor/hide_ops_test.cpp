#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/hide_ops.hpp"
#include "core/strings.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

using namespace editor_test;

namespace {
std::string bytes(const Document& doc) {
    auto result = doc.write();
    REQUIRE(result);
    return *result;
}
std::set<NodeId> objects(const Document& doc) {
    std::set<NodeId> result;
    for (const auto h : nodes(doc)) {
        if (std::holds_alternative<Handle<Solid>>(h) || std::holds_alternative<Handle<Entity>>(h))
            result.insert(id(doc, h));
    }
    return result;
}
std::multiset<std::pair<std::string, int>> vmfObjects(const Document& doc) {
    std::multiset<std::pair<std::string, int>> result;
    for (const auto node : objects(doc)) {
        const auto* n = doc.get(*doc.handle(node));
        result.emplace(n->block->key(), vmfId(doc, node).value_or(-1));
    }
    return result;
}
std::size_t wrappers(const Document& doc) {
    std::size_t result = 0;
    for (const auto h : nodes(doc)) result += ciEquals(doc.get(h)->block->key(), "hidden");
    return result;
}
NodeId parent(const Document& doc, NodeId node) { return doc.get(*doc.handle(node))->parent; }
void checkWrapped(const Document& doc, NodeId node, NodeId originalParent) {
    const auto wrapper = parent(doc, node);
    REQUIRE(wrapper != 0);
    CHECK(doc.get(*doc.handle(wrapper))->block->key() == "hidden");
    CHECK(parent(doc, wrapper) == originalParent);
    CHECK_FALSE(isVisible(doc, node));
}
}

TEST_CASE("Hide world solids, brush and point entities matches Hammer and restores selection") {
    Document doc(parsed("world { id 1 classname worldspawn }"));
    const auto world = *worldHandle(doc);
    const auto worldId = id(doc, world);
    const auto a = id(doc, addBox(doc, {0, 0, 0}, {8, 8, 8}, world));
    const auto brush = addBlock(doc, "entity { classname func_detail } ");
    const auto b = id(doc, addBox(doc, {16, 0, 0}, {24, 8, 8}, brush));
    const auto brushId = id(doc, brush);
    const auto point = id(doc, addBlock(doc, "entity { classname info_target origin \"32 0 0\" }"));
    EditorSession session;
    session.attach(doc);
    session.selection().setMode(SelectionMode::Objects);
    for (const auto node : {a, brushId, point}) session.selection().add(node);
    const auto selection = session.selection().toBlob();
    const auto original = bytes(doc);
    const auto ids = objects(doc);
    const auto undo = doc.undoCount();
    REQUIRE(session.perform("view.hide"));
    CHECK(doc.undoCount() == undo + 1);
    CHECK(doc.undoLabels().back() == "Hide");
    CHECK(session.selection().ordered().empty());
    checkWrapped(doc, a, worldId);
    checkWrapped(doc, brushId, 0);
    checkWrapped(doc, b, brushId);
    checkWrapped(doc, point, 0);
    CHECK(wrappers(doc) == 4);
    CHECK(session.picks().pick({{-10, 4, 4}, {1, 0, 0}}).empty());
    const auto hidden = bytes(doc);
    REQUIRE(session.perform("edit.undo"));
    CHECK(bytes(doc) == original);
    CHECK(session.selection().toBlob() == selection);
    REQUIRE(session.perform("edit.redo"));
    CHECK(bytes(doc) == hidden);
    CHECK(session.selection().ordered().empty());
    REQUIRE(session.perform("view.unhide"));
    CHECK(doc.undoLabels().back() == "Unhide all");
    CHECK(wrappers(doc) == 0);
    CHECK(objects(doc) == ids);
    for (const auto node : ids) CHECK(isVisible(doc, node));
    CHECK(session.selection().contains(a));
    CHECK(session.selection().contains(brushId));
    CHECK(session.selection().contains(point));
    REQUIRE(session.perform("edit.undo"));
    CHECK(bytes(doc) == hidden);
    CHECK(session.selection().ordered().empty());
    REQUIRE(session.perform("edit.redo"));
    CHECK(session.selection().contains(brushId));
}

TEST_CASE("Hide expands nested flat groups and hide unselected preserves their members") {
    Document doc(parsed("world { id 1 group { id 10 } group { id 11 editor { groupid 10 } } }"));
    const auto world = *worldHandle(doc);
    const auto a = addBox(doc, {0, 0, 0}, {8, 8, 8}, world);
    const auto b = addBox(doc, {16, 0, 0}, {24, 8, 8}, world);
    const auto c = addBox(doc, {32, 0, 0}, {40, 8, 8}, world);
    {
        auto tx = doc.begin("membership");
        tx.setEditor(a, EditorEdit::setGroup(10));
        tx.setEditor(b, EditorEdit::setGroup(11));
        tx.commit();
    }
    EditorSession session;
    session.attach(doc);
    const auto outer = *session.groups().group(10);
    session.selection().select(outer);
    const auto original = bytes(doc);
    const auto history = doc.undoCount();
    REQUIRE(session.toggleIsolate());
    CHECK(isVisible(doc, id(doc, a), session.viewState()));
    CHECK(isVisible(doc, id(doc, b), session.viewState()));
    CHECK_FALSE(isVisible(doc, id(doc, c), session.viewState()));
    CHECK(doc.undoCount() == history);
    REQUIRE(session.toggleIsolate());
    REQUIRE(session.perform("view.hide_unselected"));
    CHECK(doc.undoLabels().back() == "Hide unselected");
    CHECK(isVisible(doc, id(doc, a)));
    CHECK(isVisible(doc, id(doc, b)));
    checkWrapped(doc, id(doc, c), id(doc, world));
    CHECK(session.selection().contains(outer));
    REQUIRE(session.perform("edit.undo"));
    CHECK(bytes(doc) == original);
    REQUIRE(session.perform("view.hide"));
    checkWrapped(doc, id(doc, a), id(doc, world));
    checkWrapped(doc, id(doc, b), id(doc, world));
    CHECK(isVisible(doc, id(doc, c)));
    REQUIRE(session.perform("view.unhide"));
    CHECK(wrappers(doc) == 0);
    CHECK(session.selection().contains(outer));
}

TEST_CASE("Unhide all removes nested and empty wrappers and resets editor visibility") {
    Document doc(parsed(R"(world { id 1 hidden { hidden { solid { id 2 editor { visgroupshown 0 visgroupautoshown 0 comments keep } } } } hidden {} }
        hidden { entity { id 3 classname func_detail hidden { solid { id 4 } } editor { visgroupautoshown 0 } } }
        entity { id 5 classname info_target editor { visgroupshown 0 } })"));
    const auto ids = objects(doc);
    const auto original = bytes(doc);
    EditorSession session;
    session.attach(doc);
    REQUIRE(session.unhideAll());
    CHECK(wrappers(doc) == 0);
    CHECK(objects(doc) == ids);
    for (const auto node : ids) CHECK(isVisible(doc, node));
    CHECK(bytes(doc).find("comments keep") != std::string::npos);
    REQUIRE(session.perform("edit.undo"));
    CHECK(bytes(doc) == original);
    REQUIRE(session.perform("edit.redo"));
    CHECK(wrappers(doc) == 0);
}

TEST_CASE("Faces hide their solid and already hidden objects are skipped") {
    Document doc(parsed("world { id 1 }"));
    const auto box = addBox(doc, {0, 0, 0}, {8, 8, 8}, worldHandle(doc));
    EditorSession session;
    session.attach(doc);
    session.selection().setMode(SelectionMode::Faces);
    const auto side = doc.get(box)->block->find("side")->find("id")->asInt();
    REQUIRE(side);
    session.selection().select(SideId{*side});
    const auto before = session.selection().toBlob();
    REQUIRE(session.hideSelection());
    CHECK(wrappers(doc) == 1);
    const auto hidden = bytes(doc);
    GroupIndex groups(doc);
    Selection selected(SelectionMode::Objects);
    selected.select(id(doc, box));
    {
        auto tx = doc.begin("already hidden");
        auto result = stageHide(doc, groups, selected, tx);
        REQUIRE(result);
        CHECK(result->items.empty());
        CHECK(tx.commit().empty());
    }
    CHECK(bytes(doc) == hidden);
    REQUIRE(session.perform("edit.undo"));
    CHECK(session.selection().toBlob() == before);
}

TEST_CASE("Isolate toggles session visibility without document edits and unhide clears it") {
    Document doc(parsed("world { id 1 solid { id 2 } hidden { solid { id 3 } } } entity { id 4 classname info_target }"));
    EditorSession session;
    session.attach(doc);
    const auto all = objects(doc);
    NodeId visible = 0, hidden = 0;
    for (const auto node : all) {
        const auto h = *doc.handle(node);
        if (!std::holds_alternative<Handle<Solid>>(h)) continue;
        if (isVisible(doc, node)) visible = node;
        else hidden = node;
    }
    const auto original = bytes(doc);
    const auto dirty = doc.dirtyNodes().size();
    CHECK_FALSE(session.toggleIsolate());
    session.selection().setMode(SelectionMode::Objects);
    session.selection().add(visible);
    session.selection().add(hidden);
    REQUIRE(session.perform("view.isolate"));
    CHECK(doc.undoCount() == 0);
    CHECK(doc.dirtyNodes().size() == dirty);
    CHECK(bytes(doc) == original);
    CHECK(isVisible(doc, visible, session.viewState()));
    CHECK_FALSE(isVisible(doc, hidden, session.viewState()));
    session.selection().clear();
    REQUIRE(session.perform("view.isolate"));
    CHECK_FALSE(session.viewState().isolate);
    session.selection().select(visible);
    REQUIRE(session.toggleIsolate());
    REQUIRE(session.perform("view.unhide"));
    CHECK_FALSE(session.viewState().isolate);
    CHECK(isVisible(doc, hidden, session.viewState()));
}

TEST_CASE("Hide corpus hidden wrappers unhide without losing objects and rehide parses") {
    namespace fs = std::filesystem;
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install || !fs::is_directory(fs::path(install) / "sdk_content/maps")) {
        MESSAGE("P2CE corpus unavailable; skipping hide corpus comparison");
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
        if (!wrappers(doc)) continue;
        const auto original = bytes(doc);
        const auto ids = objects(doc);
        {
            auto tx = doc.begin("Unhide all");
            REQUIRE(stageUnhideAll(doc, tx));
            tx.commit();
        }
        CHECK(wrappers(doc) == 0);
        CHECK(objects(doc) == ids);
        Selection selected(SelectionMode::Objects);
        for (const auto h : nodes(doc)) {
            if (std::holds_alternative<Handle<Solid>>(h) && isVisible(doc, id(doc, h))) {
                selected.add(id(doc, h));
                if (selected.ordered().size() == 8) break;
            }
        }
        GroupIndex groups(doc);
        {
            auto tx = doc.begin("Hide");
            REQUIRE(stageHide(doc, groups, selected, tx));
            tx.commit();
        }
        auto reparsed = vmf::parse(bytes(doc));
        if (!reparsed) reparsed = vmf::parse(bytes(doc), {kv1::EscapeMode::Legacy});
        REQUIRE(reparsed);
        Document roundTrip(*reparsed);
        CHECK(bytes(roundTrip) == bytes(doc));
        CHECK(wrappers(roundTrip) == wrappers(doc));
        CHECK(vmfObjects(roundTrip) == vmfObjects(doc));
        REQUIRE(doc.jumpTo(0));
        CHECK(bytes(doc) == original);
    }
}

TEST_CASE("Unhide all keeps wrapper positions and selects independently hidden brush solids") {
    Document doc(parsed(R"(world { id 1 solid { id 2 } hidden { solid { id 3 } solid { id 4 } }
        hidden {} solid { id 5 } hidden { hidden { solid { id 6 } } } }
        entity { id 7 classname func_detail hidden { solid { id 8 } } })"));
    const auto world = *worldHandle(doc);
    NodeId brushSolid = 0;
    for (const auto h : nodes(doc)) {
        if (vmfId(doc, id(doc, h)) == 8) brushSolid = id(doc, h);
    }
    EditorSession session;
    session.attach(doc);
    session.selection().setMode(SelectionMode::Solids);
    REQUIRE(session.unhideAll());
    CHECK(wrappers(doc) == 0);
    std::vector<int> order;
    for (const auto child : doc.get(world)->children) order.push_back(*vmfId(doc, child));
    CHECK(order == std::vector<int>{2, 3, 4, 5, 6});
    CHECK(session.selection().contains(brushSolid));
}

TEST_CASE("Isolate a brush solid keeps its sibling out while Hide uses its object owner") {
    Document doc(parsed("world { id 1 } entity { id 2 classname func_detail }"));
    NodeHandle brush = *doc.handle(doc.roots().back());
    const auto a = id(doc, addBox(doc, {0, 0, 0}, {8, 8, 8}, brush));
    const auto b = id(doc, addBox(doc, {16, 0, 0}, {24, 8, 8}, brush));
    EditorSession session;
    session.attach(doc);
    session.selection().setMode(SelectionMode::Solids);
    session.selection().select(a);
    const auto before = bytes(doc);
    const auto history = doc.undoCount();
    REQUIRE(session.toggleIsolate());
    CHECK(isVisible(doc, a, session.viewState()));
    CHECK_FALSE(isVisible(doc, b, session.viewState()));
    CHECK(doc.undoCount() == history);
    CHECK(bytes(doc) == before);
    REQUIRE(session.toggleIsolate());
    REQUIRE(session.hideSelection());
    checkWrapped(doc, id(doc, brush), 0);
    checkWrapped(doc, a, id(doc, brush));
    checkWrapped(doc, b, id(doc, brush));
    REQUIRE(session.perform("edit.undo"));
    CHECK(bytes(doc) == before);
    session.selection().setMode(SelectionMode::Faces);
    const auto side = doc.get(*doc.handle(a))->block->find("side")->find("id")->asInt();
    REQUIRE(side);
    session.selection().select(SideId{*side});
    REQUIRE(session.hideSelection());
    CHECK(wrappers(doc) == 1);
    CHECK(isVisible(doc, b));
    checkWrapped(doc, a, id(doc, brush));
}
