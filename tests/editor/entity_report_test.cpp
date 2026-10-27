#include "fixtures.hpp"
#include "editor/entity_report.hpp"

#include <algorithm>

using namespace editor_test;

TEST_CASE("Entity report filters match authored fixture hand counts and sorted selection ids") {
    Document doc(parsed(R"(
world { classname worldspawn }
entity { classname prop_dynamic targetname "Door_A" model "models/door.mdl" origin "0 0 0" editor { visgroupid 7 } }
entity { classname prop_static targetname "Door_B" origin "100 0 0" }
entity { classname logic_relay targetname "Relay" origin "500 0 0" editor { visgroupid 7 } }
entity { classname func_detail targetname "Brush" }
visgroups { visgroup { name "Test" visgroupid 7 } }
)"));
    const auto brush = doc.roots()[4];
    addBox(doc, {-16,-16,-16}, {16,16,16}, doc.handle(brush));
    EditorSession session;
    session.attach(doc);
    auto rows = entityReport(session);
    REQUIRE(rows.size() == 4);
    CHECK(std::is_sorted(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.entity < b.entity; }));
    EntityReportFilter filter;
    filter.classname = "PROP_*";
    CHECK(entityReport(session, filter).size() == 2);
    filter.classname = "prop_dynamic";
    CHECK(entityReport(session, filter).size() == 1);
    filter = {}; filter.name = "door";
    CHECK(entityReport(session, filter).size() == 2);
    filter = {}; filter.keyContains = "model"; filter.searchValues = false;
    CHECK(entityReport(session, filter).size() == 1);
    filter = {}; filter.keyContains = "door.mdl"; filter.searchKeys = false;
    CHECK(entityReport(session, filter).size() == 1);
    filter = {}; filter.visgroup = 7;
    CHECK(entityReport(session, filter).size() == 2);
    filter = {}; filter.brush = true;
    REQUIRE(entityReport(session, filter).size() == 1);
    CHECK(entityReport(session, filter)[0].entity == brush);
    filter.brush = false;
    CHECK(entityReport(session, filter).size() == 3);
    filter = {}; filter.insideSelection = true;
    CHECK(entityReport(session, filter).empty());
    session.selection().add(brush);
    CHECK(entityReport(session, filter).size() == 2); // brush plus enclosed Door_A
    filter.name = "door";
    CHECK(entityReport(session, filter).size() == 1);
    auto selection = entityReportSelection(rows);
    CHECK(selection.mode() == SelectionMode::Objects);
    CHECK(selection.ordered().size() == 4);
    CHECK(entityReportIds(rows).size() == 4);
    for (const auto& row : rows) CHECK(selection.contains(row.entity));
}
