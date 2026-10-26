#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/name_index.hpp"

#include <stdexcept>

using namespace editor_test;

TEST_CASE("NameIndex builds case insensitive sorted unique entity names and wildcard matches") {
    Document doc(parsed(R"(
        entity { targetname Door_A }
        entity { targetname DOOR_A }
        entity { targetname door_b }
        entity { targetname preMIDpost }
        entity { targetname other_a }
        entity { classname info_target }
        entity { targetname "" }
        solid { targetname door_solid }
    )"));
    NameIndex index(doc);
    const auto roots = doc.roots();
    CHECK(index.size() == 4);
    CHECK(index.names() == std::vector<std::string>{"Door_A", "door_b", "other_a", "preMIDpost"});
    CHECK(index.find("dOoR_a") == std::vector<NodeId>{roots[0], roots[1]});
    CHECK(index.find("").empty());
    CHECK(index.find("missing").empty());
    CHECK(index.matching("DOOR*") == std::vector<NodeId>{roots[0], roots[1], roots[2]});
    CHECK(index.matching("*_a") == std::vector<NodeId>{roots[0], roots[1], roots[4]});
    CHECK(index.matching("*mid*") == std::vector<NodeId>{roots[3]});
    CHECK(index.matching("door_a") == index.find("door_a"));
    CHECK(index.matching("*") == std::vector<NodeId>{roots[0], roots[1], roots[2], roots[3], roots[4]});
    CHECK(index.matching("d*or").empty());
    CHECK(index.matching("***").empty());
}

TEST_CASE("NameIndex ChangeSets track creation rename deletion undo redo and stable NodeIds") {
    Document doc(parsed("entity { targetname Door_A } entity { targetname door_a } entity {}"));
    const auto a = doc.roots()[0], b = doc.roots()[1], unnamed = doc.roots()[2];
    NameIndex index(doc);
    index.attach(doc);
    index.attach(doc);
    auto rename = doc.begin("rename");
    rename.setKeyValue(*doc.handle(a), "targetname", "Door_B");
    rename.setKeyValue(*doc.handle(unnamed), "targetname", "New");
    rename.commit();
    CHECK(index.find("door_a") == std::vector<NodeId>{b});
    CHECK(index.find("door_b") == std::vector<NodeId>{a});
    CHECK(index.find("NEW") == std::vector<NodeId>{unnamed});
    CHECK(index.names() == std::vector<std::string>{"Door_A", "Door_B", "New"});
    const auto created = addBlock(doc, "entity { targetname DOOR_B }");
    const auto c = id(doc, created);
    CHECK(index.find("door_b") == std::vector<NodeId>{a, c});
    const auto oldHandle = *doc.handle(a);
    auto erase = doc.begin("delete");
    erase.destroy(oldHandle);
    erase.commit();
    CHECK(index.find("door_b") == std::vector<NodeId>{c});
    REQUIRE(doc.undo());
    CHECK(*doc.handle(a) != oldHandle);
    CHECK(index.find("door_b") == std::vector<NodeId>{a, c});
    REQUIRE(doc.redo());
    CHECK(index.find("door_b") == std::vector<NodeId>{c});
    REQUIRE(doc.undo());
    REQUIRE(doc.undo());
    REQUIRE(doc.undo());
    CHECK(index.find("door_a") == std::vector<NodeId>{a, b});
    CHECK(index.find("new").empty());
    REQUIRE(doc.redo());
    CHECK(index.find("door_b") == std::vector<NodeId>{a});
    NameIndex rebuilt(doc);
    // Display spelling stays first-seen while the name remains indexed.
    CHECK(index.names() == std::vector<std::string>{"Door_A", "Door_B", "New"});
    for (const auto& name : rebuilt.names()) CHECK(index.find(name) == rebuilt.find(name));
    CHECK(index.matching("*") == rebuilt.matching("*"));
    index.detach();
    auto clear = doc.begin("clear");
    clear.setKeyValue(*doc.handle(a), "targetname", "");
    clear.commit();
    CHECK(index.find("door_b") == std::vector<NodeId>{a});
    index.attach(doc);
    CHECK(index.find("door_b").empty());
    Document other(parsed());
    CHECK_THROWS_AS(index.attach(other), std::invalid_argument);
    { NameIndex temporary(doc); temporary.attach(doc); }
    const auto extra = addBlock(doc, "entity { targetname Extra }");
    CHECK(index.find("extra") == std::vector<NodeId>{id(doc, extra)});
}

TEST_CASE("session NameIndex follows document edits and reattachment") {
    Document doc(parsed("entity { targetname First }"));
    Document other(parsed("entity { targetname Second }"));
    EditorSession session;
    session.attach(doc);
    CHECK(session.names().find("first") == doc.roots());
    auto tx = doc.begin("rename");
    tx.setKeyValue(*doc.handle(doc.roots().front()), "targetname", "Renamed");
    tx.commit();
    CHECK(session.names().find("first").empty());
    CHECK(session.names().find("renamed") == doc.roots());
    REQUIRE(session.perform("edit.undo"));
    CHECK(session.names().find("first") == doc.roots());
    REQUIRE(session.perform("edit.redo"));
    CHECK(session.names().find("renamed") == doc.roots());
    session.attach(other);
    CHECK(session.names().find("renamed").empty());
    CHECK(session.names().find("second") == other.roots());
    session.detach();
}
