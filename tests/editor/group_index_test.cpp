#include "fixtures.hpp"
#include "editor/group_index.hpp"

#include <stdexcept>

using namespace editor_test;

namespace {
int vmfId(const Document& doc, NodeHandle h) { return doc.get(h)->block->find("id")->asInt().value(); }
void join(Document& doc, NodeHandle object, std::optional<int> group) {
    auto tx = doc.begin("membership");
    tx.setEditor(object, EditorEdit::setGroup(group));
    tx.commit();
}
}

TEST_CASE("GroupIndex flat nested membership and incremental observer match rebuild") {
    Document doc(parsed());
    const auto outer = addBlock(doc, "group {}");
    const auto inner = addBlock(doc, "group {}");
    const auto solid = addBox(doc, DVec3{-8}, DVec3{8});
    const auto entity = addBlock(doc, "entity { classname info_target }");
    GroupIndex groups(doc);
    groups.attach(doc);
    groups.attach(doc);
    join(doc, inner, vmfId(doc, outer));
    join(doc, solid, vmfId(doc, inner));
    join(doc, entity, vmfId(doc, outer));
    CHECK(groups.size() == 2);
    CHECK(groups.membershipCount() == 3);
    CHECK(groups.parentGroup(id(doc, solid)) == id(doc, inner));
    CHECK(groups.outermostGroup(id(doc, solid)) == id(doc, outer));
    CHECK(groups.outermostGroup(id(doc, outer)) == id(doc, outer));
    CHECK(groups.members(id(doc, outer)) == std::vector<NodeId>{id(doc, inner), id(doc, entity)});
    CHECK(groups.members(id(doc, outer), true) ==
          std::vector<NodeId>{id(doc, inner), id(doc, solid), id(doc, entity)});
    join(doc, solid, {});
    CHECK_FALSE(groups.outermostGroup(id(doc, solid)));
    REQUIRE(doc.undo());
    CHECK(groups.outermostGroup(id(doc, solid)) == id(doc, outer));
    REQUIRE(doc.redo());
    CHECK_FALSE(groups.outermostGroup(id(doc, solid)));
    // Direct editor-child key edits and removal update their owner too.
    const auto editor = *doc.handle(doc.get(entity)->children.front());
    auto tx = doc.begin("generic membership edit");
    tx.setKeyValue(editor, "groupid", std::to_string(vmfId(doc, inner)));
    tx.commit();
    CHECK(groups.parentGroup(id(doc, entity)) == id(doc, inner));
    auto eraseEditor = doc.begin("erase editor");
    eraseEditor.destroy(editor);
    eraseEditor.commit();
    CHECK_FALSE(groups.parentGroup(id(doc, entity)));
    REQUIRE(doc.undo());
    CHECK(groups.parentGroup(id(doc, entity)) == id(doc, inner));
    const auto innerId = id(doc, inner);
    auto erase = doc.begin("erase group");
    erase.destroy(inner);
    erase.commit();
    CHECK(groups.size() == 1);
    CHECK_FALSE(groups.parentGroup(id(doc, entity)));
    REQUIRE(doc.undo());
    CHECK(groups.parentGroup(id(doc, entity)) == innerId);
    CHECK(*doc.handle(innerId) != inner);
    GroupIndex rebuilt(doc);
    CHECK(groups.members(id(doc, outer), true) == rebuilt.members(id(doc, outer), true));
    groups.detach();
    join(doc, solid, vmfId(doc, outer));
    CHECK_FALSE(groups.parentGroup(id(doc, solid)));
    groups.attach(doc);
    CHECK(groups.parentGroup(id(doc, solid)) == id(doc, outer));
    { GroupIndex temporary(doc); temporary.attach(doc); }
    join(doc, solid, {});
    Document other(parsed());
    CHECK_THROWS_AS(groups.attach(other), std::invalid_argument);
}

TEST_CASE("GroupIndex resolves creation dangling references hierarchy and case insensitive integer keys") {
    Document doc(parsed("world { id 1 solid { id 2 editor { GrOuPiD 1 } } }"));
    const auto world = *doc.handle(doc.roots().front());
    const auto solid = *doc.handle(doc.get(world)->children.front());
    GroupIndex groups(doc);
    groups.attach(doc);
    CHECK_FALSE(groups.parentGroup(id(doc, solid)));
    const auto group = addBlock(doc, "group { ID 3 }", world);
    // Creation allocates VMF ID 1 and resolves the formerly dangling membership.
    CHECK(groups.parentGroup(id(doc, solid)) == id(doc, group));
    join(doc, solid, vmfId(doc, group));
    CHECK(groups.members(id(doc, group)) == std::vector<NodeId>{id(doc, solid)});
    const auto editor = addBlock(doc, "EdItOr { GROUPID 1 }");
    auto move = doc.begin("move editor");
    move.reparent(editor, group);
    move.commit();
    // A structural group parent never creates membership.
    const auto child = addBlock(doc, "entity { classname info_target }", group);
    CHECK_FALSE(groups.parentGroup(id(doc, child)));
    GroupIndex rebuilt(doc);
    CHECK(groups.membershipCount() == rebuilt.membershipCount());
    CHECK(groups.outermostGroup(id(doc, group)) == rebuilt.outermostGroup(id(doc, group)));
}

TEST_CASE("GroupIndex malformed cycles missing IDs duplicate IDs and dangling memberships terminate") {
    Document doc(parsed(R"(
        group { id 10 editor { groupid 20 } }
        group { id 20 editor { groupid 10 } }
        group { id 30 editor { groupid 30 } }
        group { id 10 }
        group { id invalid }
        entity { id 1 editor { groupid 10 } }
        entity { id 2 editor { groupid 999 } }
        entity { id 3 editor { groupid invalid } }
    )"));
    GroupIndex groups(doc);
    REQUIRE(groups.group(10));
    const auto a = *groups.group(10), b = *groups.group(20), c = *groups.group(30);
    CHECK(a == doc.roots()[0]);
    CHECK_FALSE(groups.parentGroup(a));
    CHECK_FALSE(groups.parentGroup(b));
    CHECK_FALSE(groups.parentGroup(c));
    CHECK(groups.outermostGroup(doc.roots()[5]) == a);
    CHECK_FALSE(groups.outermostGroup(doc.roots()[6]));
    CHECK_FALSE(groups.outermostGroup(doc.roots()[7]));
    CHECK(groups.members(a, true) == std::vector<NodeId>{doc.roots()[5]});
    CHECK(groups.members(b, true).empty());
    for (const auto h : nodes(doc)) CHECK(groups.members(doc.get(h)->id, true).size() <= nodes(doc).size());
    groups.attach(doc);
    auto repair = doc.begin("repair cycle");
    repair.setEditor(*doc.handle(a), EditorEdit::setGroup({}));
    repair.commit();
    CHECK(groups.parentGroup(b) == a);
    CHECK(groups.outermostGroup(b) == a);
    REQUIRE(doc.undo());
    CHECK_FALSE(groups.parentGroup(a));
    CHECK_FALSE(groups.parentGroup(b));
    auto erase = doc.begin("remove first duplicate definition");
    erase.destroy(*doc.handle(a));
    erase.commit();
    CHECK(groups.group(10) == doc.roots()[2]);
    CHECK(groups.outermostGroup(b) == groups.group(10));
    GroupIndex rebuilt(doc);
    CHECK(groups.membershipCount() == rebuilt.membershipCount());
    CHECK(groups.members(*groups.group(10), true) == rebuilt.members(*rebuilt.group(10), true));
}

TEST_CASE("GroupIndex created and destroyed members refresh inline and managed metadata") {
    Document doc(parsed());
    const auto group = addBlock(doc, "group {}");
    const auto groupId = id(doc, group);
    const auto number = vmfId(doc, group);
    GroupIndex groups(doc);
    groups.attach(doc);
    const auto solid = addBlock(doc, "solid { editor { groupid " + std::to_string(number) + " } }");
    const auto solidId = id(doc, solid);
    const auto point = addBlock(doc, "entity { editor { groupid " + std::to_string(number) + " } }");
    const auto pointId = id(doc, point);
    CHECK(groups.members(groupId) == std::vector<NodeId>{solidId, pointId});
    auto tx = doc.begin("erase members");
    tx.destroy(solid);
    tx.destroy(point);
    tx.commit();
    CHECK(groups.members(groupId).empty());
    CHECK(groups.membershipCount() == 0);
    REQUIRE(doc.undo());
    CHECK(groups.members(groupId) == std::vector<NodeId>{solidId, pointId});
    REQUIRE(doc.redo());
    CHECK(groups.members(groupId).empty());
}
