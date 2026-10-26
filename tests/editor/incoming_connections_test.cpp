#include "fixtures.hpp"
#include "editor/incoming_connections.hpp"

using namespace editor_test;

TEST_CASE("incoming connections union exact wildcard classname and self targets in document order") {
    Document doc(parsed(R"(entity { id 1 classname logic_relay targetname relay connections {
        OnExact "Door_A,Open,,0,-1"
        OnCase "door_a,Open,,0,-1"
        OnWild "*Door_A,Close,,0,-1"
        OnClass "prop_door_rotating,Open,,0,-1"
        OnSelf "relay,Trigger,,0,-1"
        OnMalformed "Door_A,broken"
        OnSpecial "!self,Open,,0,-1"
    } }
    entity { id 2 classname prop_door_rotating targetname Door_A }
    entity { id 3 classname info_target targetname prefix_Door_A })"));
    NameIndex names(doc);
    assets::EntityClassTable classes;
    ConnectionValidator validator(doc, names, classes);
    const std::vector<NodeId> targets{doc.get(*doc.handle(doc.roots()[1]))->id,
                                      doc.get(*doc.handle(doc.roots()[2]))->id,
                                      doc.get(*doc.handle(doc.roots()[0]))->id};
    const auto incoming = incomingConnections(validator, doc, targets);
    REQUIRE(incoming.size() == 5);
    CHECK(incoming[0].output == "OnExact");
    CHECK(incoming[0].connection.target == "Door_A");
    CHECK(incoming[1].output == "OnCase");
    CHECK(incoming[2].output == "OnWild");
    CHECK(incoming[3].output == "OnClass");
    CHECK(incoming[4].output == "OnSelf");
    CHECK(incoming[4].source == targets[2]);
}
