#include "viewport/workspace.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::viewport;

TEST_CASE("area roles keep the viewport and steer the active pane") {
    ViewportLayout l;
    const NodeId a = l.paneIds().front();
    const NodeId b = *l.split(a, Orientation::Horizontal);
    l.viewport(b)->camera.orthoHeight = 321;
    CHECK(l.role(b) == AreaRole::Viewport);
    REQUIRE(l.setActive(b));
    REQUIRE(l.setRole(b, AreaRole::Outliner));
    CHECK(l.viewport(b) == nullptr);
    CHECK(l.active() == a);  // The active viewport moved off the panel.
    CHECK_FALSE(l.setActive(b));
    CHECK(l.viewportIds() == std::vector<NodeId>{a});
    CHECK(l.paneIds(AreaRole::Outliner) == std::vector<NodeId>{b});
    REQUIRE(l.setRole(b, AreaRole::Viewport));
    CHECK(l.viewport(b)->camera.orthoHeight == 321);

    REQUIRE(l.setRole(a, AreaRole::Log));
    REQUIRE(l.setRole(b, AreaRole::Inspector));
    CHECK(l.active() == 0);  // No viewport left.
}

TEST_CASE("collapse keeps one area expanded and survives close and JSON") {
    ViewportLayout l;
    const NodeId a = l.paneIds().front();
    const NodeId b = *l.split(a, Orientation::Horizontal, 0.3);
    REQUIRE(l.setRole(a, AreaRole::Outliner));
    REQUIRE(l.setCollapsed(a, true));
    CHECK(l.collapsed(a));
    CHECK_FALSE(l.setCollapsed(b, true));  // The last expanded area.

    const auto back = ViewportLayout::fromJson(l.toJson());
    REQUIRE(back);
    CHECK(back->role(a) == AreaRole::Outliner);
    CHECK(back->collapsed(a));
    CHECK_FALSE(back->collapsed(b));
    CHECK(back->tree().ratio == doctest::Approx(0.3));

    REQUIRE(l.close(b));
    CHECK_FALSE(l.collapsed(a));  // The survivor expands.
}

TEST_CASE("version-1 layouts read as viewports") {
    auto parsed = json::parse(R"({"version":1,"active":1,"root":{"id":1,"type":"pane","viewport":)" +
                              json::write(viewport::toJson(makePreset(Preset::Top))) + "}}");
    REQUIRE(parsed);
    const auto l = ViewportLayout::fromJson(*parsed);
    REQUIRE(l);
    CHECK(l->role(1) == AreaRole::Viewport);
    CHECK(l->active() == 1);

    auto badRole = json::parse(R"({"root":{"id":1,"type":"pane","role":"teapot","viewport":)" +
                               json::write(viewport::toJson(makePreset(Preset::Top))) + "}}");
    REQUIRE(badRole);
    CHECK_FALSE(ViewportLayout::fromJson(*badRole));
}

TEST_CASE("workspace templates and set editing") {
    const auto layout = workspaceTemplate("Layout");
    CHECK(layout.viewportIds().size() == 4);
    for (const auto role : {AreaRole::Outliner, AreaRole::Inspector, AreaRole::Visgroups, AreaRole::History, AreaRole::Log})
        CHECK(layout.paneIds(role).size() == 1);
    CHECK(layout.viewport(layout.active())->camera.projection == ProjectionKind::Perspective);
    const auto four = workspaceTemplate("Four Views");
    CHECK(four.viewportIds().size() == 4);
    CHECK(four.collapsed(four.paneIds(AreaRole::Outliner).front()));
    CHECK(workspaceTemplate("Modeling").viewportIds().size() == 1);

    auto set = WorkspaceSet::defaults();
    REQUIRE(set.workspaces.size() == workspaceTemplateNames().size());
    CHECK(set.uniqueName("Layout") == "Layout 2");
    CHECK(set.uniqueName("Mine") == "Mine");
    set.active = 2;
    REQUIRE(set.move(2, 0));
    CHECK(set.active == 0);
    CHECK(set.workspaces[0].name == "Four Views");
    REQUIRE(set.remove(0));
    CHECK(set.active == 0);
    REQUIRE(set.remove(1));
    CHECK_FALSE(set.remove(0));

    set = WorkspaceSet::defaults();
    set.active = 1;
    const auto back = WorkspaceSet::fromJson(set.toJson());
    REQUIRE(back);
    CHECK(back->active == 1);
    REQUIRE(back->workspaces.size() == set.workspaces.size());
    CHECK(back->workspaces[1].name == "Modeling");
    CHECK(back->workspaces[2].base == "Four Views");
    CHECK(back->workspaces[0].layout.paneIds() == set.workspaces[0].layout.paneIds());
    json::Value empty;
    empty.data = json::Value::Object{};
    CHECK_FALSE(WorkspaceSet::fromJson(empty));
}
