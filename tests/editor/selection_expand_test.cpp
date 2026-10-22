#include "fixtures.hpp"
#include "editor/selection_expand.hpp"

using namespace editor_test;

TEST_CASE("expandSelection resolves nested groups and brush entities to drawable IDs") {
    Document doc(parsed(R"(
world { "id" "1"
 group { "id" "10" }
 group { "id" "11" editor { "groupid" "10" } }
 solid { "id" "2" editor { "groupid" "11" } }
}
entity { "id" "3" editor { "groupid" "10" }
 hidden { solid { "id" "4" side { "id" "100" } } }
}
entity { "id" "5" editor { "groupid" "11" } }
)"));
    GroupIndex groups(doc);
    Selection selection;
    selection.select(*groups.group(10));
    auto expanded = expandSelection(doc, groups, selection);
    std::set<NodeId> expected;
    NodeId brushEntity = 0;
    for (const auto h : nodes(doc)) {
        const auto* n = doc.get(h);
        if (std::holds_alternative<Handle<Solid>>(h)) expected.insert(n->id);
        if (const auto* number = n->block->find("id"); number && number->asInt() == 5) expected.insert(n->id);
        if (const auto* number = n->block->find("id"); number && number->asInt() == 3) brushEntity = n->id;
    }
    CHECK(std::set<NodeId>(expanded.solidsAndEntities.begin(), expanded.solidsAndEntities.end()) == expected);
    CHECK(expanded.solidsAndEntities.size() == 3);
    selection.add(brushEntity);
    CHECK(expandSelection(doc, groups, selection).solidsAndEntities.size() == 3);
    selection.setMode(SelectionMode::Objects);
    selection.select(brushEntity);
    CHECK(expandSelection(doc, groups, selection).solidsAndEntities.size() == 1);
    selection.clear();
    const auto child = expanded.solidsAndEntities.back();
    selection.add(child);
    selection.add(brushEntity);
    selection.add(NodeId{9999});
    CHECK(expandSelection(doc, groups, selection).solidsAndEntities == std::vector<NodeId>{child});
    selection.setMode(SelectionMode::Faces);
    selection.add(SideId{100});
    selection.add(SideId{999});
    expanded = expandSelection(doc, groups, selection);
    CHECK(expanded.sides == std::vector<SideId>{100});
    CHECK(expanded.solidsAndEntities.empty());
}
