#include "plugin_host/plugin_host.hpp"
#include "plugin_host/plugin_tool.hpp"
#include "document/vmf.hpp"
#include "document/document.hpp"
#include "editor/editor_session.hpp"
#include "editor/map_problems.hpp"
#include "../editor/fixtures.hpp"
#include "geometry/solid_geometry.hpp"
#include <doctest/doctest.h>
#include <apex/plugin.hpp>
#include "editor/selection_undo.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <thread>

using namespace apex;
using namespace apex::plugin_host;
TEST_CASE("plugin ui registration describes and refreshes a panel") {
    struct Backend : UiBackend {
        std::string refreshed; Host* host = nullptr; bool accept = true; std::string dialogTitle;
        void formChanged(std::string_view id) override { refreshed = id; }
        bool runDialog(std::string_view id, std::string_view title) override {
            dialogTitle = title;
            size_t count = 0;
            const bool described = host->describePanel(id, [&](const apex_ui_form& form) { count = form.control_count; });
            REQUIRE(described); CHECK(count == 1);
            apex_ui_value value{}; value.struct_size = sizeof(value);
            host->panelChanged(id, "d", value);
            return accept;
        }
    } backend;
    const auto path = std::filesystem::temp_directory_path() / "apex-ui-host-test";
    std::filesystem::create_directories(path / "ui");
    {
        std::ofstream out(path / "ui" / "plugin.kv3");
#ifdef _WIN32
        constexpr auto platform = "windows-x86_64";
#else
        constexpr auto platform = "linux-x86_64";
#endif
        out << "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n"
            << "{ id = \"ui\" name = \"UI\" apex_abi = { major = 1 minor = 0 } entry = { \""
            << platform << "\" = \"" << UI_PLUGIN_PATH << "\" } }";
    }
    std::vector<std::string> logs, panels;
    {
        Host host(path / "state", path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
        host.setPanelRegistrar([&](std::string id, std::string) { panels.push_back(std::move(id)); return true; });
        host.setUiBackend(&backend); backend.host = &host;
        host.discover({path}); host.activate();
        REQUIRE(host.plugins().size() == 1);
        CAPTURE(host.plugins()[0].error);
        REQUIRE(host.plugins()[0].loaded);
        REQUIRE(panels == std::vector<std::string>{"plugin.ui.panel"});
        size_t count = 0; std::string firstKey;
        const bool described = host.describePanel("plugin.ui.panel", [&](const apex_ui_form& form) {
            count = form.control_count; firstKey.assign(form.controls[0].key.data, form.controls[0].key.size);
        });
        REQUIRE(described);
        CHECK(count == 2); CHECK(firstKey == "field");
        apex_ui_value value{}; value.struct_size = sizeof(value);
        host.panelChanged("plugin.ui.panel", "field", value);
        CHECK(backend.refreshed == "plugin.ui.panel");
        host.panelClicked("plugin.ui.panel", "field");
        CHECK(logs == std::vector<std::string>{"changed", "clicked"});
        logs.clear();
        host.panelClicked("plugin.ui.panel", "open");
        CHECK(backend.dialogTitle == "Options");
        CHECK(logs == std::vector<std::string>{"dialog changed", "dialog accepted"});
        logs.clear(); backend.accept = false;
        host.panelClicked("plugin.ui.panel", "open");
        CHECK(logs == std::vector<std::string>{"dialog changed", "dialog cancelled"});
        // Sections are not area roles; they receive the context the host panel names.
        REQUIRE(host.sections("materials").size() == 1);
        CHECK(host.sections("inspector").size() == 1);
        CHECK(host.sections("face_properties").size() == 1);
        CHECK(host.sections("nothing").empty());
        CHECK(host.hasPanel("plugin.ui.panel"));
        CHECK_FALSE(host.hasPanel("plugin.ui.section"));
        // Names longer than the small-string buffer, so a copy freed before the plugin reads them shows up.
        UiContext context{"materials", {"dev/a_material_name_past_sso", "dev/b_material_name_past_sso"}, {}};
        std::string summary;
        const auto describeSection = [&](const UiContext& c) {
            return host.describePanel("plugin.ui.section", &c, [&](const apex_ui_form& form) {
                summary = std::string(form.controls[0].value.text.data, form.controls[0].value.text.size);
            });
        };
        REQUIRE(describeSection(context));
        CHECK(summary == "materials names=2 ids=0 first=dev/a_material_name_past_sso");
        UiContext sides{"sides", {}, {7, 9, 11}};
        REQUIRE(describeSection(sides));
        CHECK(summary == "sides names=0 ids=3 first=");
        CHECK_FALSE(host.describePanel("plugin.ui.section", [](const apex_ui_form&) {}));  // sections need a context
    }
    std::filesystem::remove_all(path);
}
namespace {
std::string entryFor(std::string_view path) {
#ifdef _WIN32
    constexpr auto platform = "windows-x86_64";
#else
    constexpr auto platform = "linux-x86_64";
#endif
    return "entry = { \"" + std::string(platform) + "\" = \"" + std::string(path) + "\" }";
}
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("apex-plugin-test-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directories(path); }
    ~Temp() { std::filesystem::remove_all(path); }
    void manifest(std::string_view id, std::string_view entry) {
        auto dir = path / id;
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "plugin.kv3") << "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n"
            << "{ id = \"" << id << "\" name = \"Example\" version = \"1.0.0\" apex_abi = { major = 1 minor = 0 } " << entry << " }";
    }
};
}
TEST_CASE("plugin namespace is enforced") {
    CHECK(inNamespace("example", "plugin.example.action"));
    CHECK_FALSE(inNamespace("example", "plugin.other.action"));
    CHECK_FALSE(inNamespace("example", "plugin.exampleish.action"));
    CHECK_FALSE(inNamespace("com.example", "plugin.com.example.paint.brush"));  // the longer id's namespace
    CHECK_FALSE(inNamespace("example", "plugin.example."));
}
TEST_CASE("plugin manifest missing platform entry remains listed") {
    Temp temp;
    temp.manifest("example", "entry = { missing = \"unused\" }");
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path});
    REQUIRE(host.plugins().size() == 1);
    CHECK(host.plugins()[0].error.find("entry") != std::string::npos);
}
TEST_CASE("plugin wrong major is refused with both versions") {
    Temp temp;
    temp.manifest("bad", entryFor(BAD_PLUGIN_PATH));
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path});
    host.activate();
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    CHECK(host.plugins()[0].error.find("99") != std::string::npos);
    CHECK(host.plugins()[0].error.find("1") != std::string::npos);
}
TEST_CASE("hello plugin reads the open document") {
    Temp temp;
    temp.manifest("hello", entryFor(HELLO_PLUGIN_PATH));
    auto file = vmf::parse("world { id 1 classname worldspawn }");
    REQUIRE(file);
    document::Document document(*file);
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
    host.discover({temp.path});
    host.setDocument(&document);
    host.activate();
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    CHECK(host.plugins()[0].error.empty());
    CHECK(host.plugins()[0].loaded);
    REQUIRE_FALSE(logs.empty());
    CHECK(logs.back().find("worldspawn") != std::string::npos);
}

TEST_CASE("plugin enable state survives restart and safe mode preserves it") {
    Temp temp;
    temp.manifest("example", "entry = { missing = \"unused\" }");
    {
        Host host(temp.path / "state", temp.path / "breadcrumb");
        host.discover({temp.path});
        host.setEnabled("example", false);
    }
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path});
    REQUIRE(host.plugins().size() == 1);
    CHECK_FALSE(host.plugins()[0].enabled);
    host.activate(true);
    CHECK_FALSE(host.plugins()[0].enabled);
}
TEST_CASE("plugin crash leaves activation breadcrumb for next startup") {
    Temp temp;
    temp.manifest("crasher", entryFor(CRASH_PLUGIN_PATH));
    const auto command = std::string("\"") + CRASH_CHILD_PATH + "\" \"" + temp.path.string() + "\"";
    CHECK(std::system(command.c_str()) != 0);
    Host next(temp.path / "state", temp.path / "breadcrumb");
    CHECK(next.staleBreadcrumb() == "crasher:activate");
}

TEST_CASE("safe mode skips an enabled plugin without changing its setting") {
    Temp temp;
    temp.manifest("hello", entryFor(HELLO_PLUGIN_PATH));
    {
        Host host(temp.path / "state", temp.path / "breadcrumb");
        host.discover({temp.path});
        REQUIRE(host.plugins().size() == 1);
        REQUIRE(host.plugins()[0].error.empty());
        host.activate(true);
        CHECK(host.plugins()[0].enabled);
        CHECK_FALSE(host.plugins()[0].loaded);
    }
    Host again(temp.path / "state", temp.path / "breadcrumb");
    again.discover({temp.path});
    CHECK(again.plugins()[0].enabled);
    again.activate();
    CHECK(again.plugins()[0].loaded);
}
TEST_CASE("hello plugin is notified when a document opens after activation") {
    Temp temp;
    temp.manifest("hello", entryFor(HELLO_PLUGIN_PATH));
    auto file = vmf::parse("world { id 1 classname worldspawn }");
    REQUIRE(file);
    document::Document document(*file);
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
    host.discover({temp.path});
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    REQUIRE(editor::actionRegistry().find("plugin.hello.read_document"));
    editor::Keymap keymap(editor::hammerPreset());
    const auto chord = editor::KeyChord::parse("F24");
    REQUIRE(chord);
    CHECK(keymap.lookup(*chord, editor::BindingContext::forTool("plugin.hello.tool")) == "plugin.hello.read_document");
    host.setDocument(&document);
    REQUIRE_FALSE(logs.empty());
    CHECK(logs.back() == "worldspawn");
    host.setDocument(nullptr);
}
TEST_CASE("document interface reads hierarchy and duplicate keyvalues") {
    Temp temp;
    temp.manifest("reader", entryFor(READ_PLUGIN_PATH));
    auto file = vmf::parse("world { id 1 message one message two group { id 2 } }");
    REQUIRE(file);
    document::Document document(*file);
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path});
    host.setDocument(&document);
    host.activate();
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    CHECK(host.plugins()[0].loaded);
}

TEST_CASE("apex.document/1 reads a solid's sides (ABI 1.1)") {
    Temp temp;
    temp.manifest("sides", entryFor(SIDE_PLUGIN_PATH));
    // Side 11's plane does not parse: it reads as zero, and its material and axes still come through.
    auto file = vmf::parse(R"vmf(world { id 1 classname worldspawn
        solid { id 2
            side { id 10 plane "(0 0 0) (0 64 0) (64 64 0)" material "CONCRETE/WALL01" uaxis "[1 0 0 16] 0.25" vaxis "[0 -1 0 -8] 0.5" }
            side { id 11 plane "not a plane" material "WOOD/FLOOR" uaxis "[0 1 0 0] 1" vaxis "[0 0 -1 4] 2" } } })vmf");
    REQUIRE(file);
    document::Document document(*file);
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
    host.discover({temp.path});
    host.setDocument(&document);
    host.activate();
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    REQUIRE(host.plugins()[0].loaded);
    CHECK(logs == std::vector<std::string>{
        "world sides 0",
        "solid sides 2",
        "side 10 CONCRETE/WALL01 p1=0,64,0 u=1,0,0/16/0.25 v=0,-1,0/-8/0.5",
        "side 11 WOOD/FLOOR p1=0,0,0 u=0,1,0/0/1 v=0,0,-1/4/2",
        "past the end: not found",
        "world side: not found",
        "short struct: invalid",
        "lookup 11: index 1, same solid 1, id 11",
        "lookup 999: not found",
    });
}

TEST_CASE("face-stats example summarises the selected sides in a face properties section") {
    Temp temp;
    temp.manifest("facestats", entryFor(FACE_STATS_PLUGIN_PATH));
    auto file = vmf::parse("world { id 1 classname worldspawn solid { id 2 } solid { id 3 } }");
    REQUIRE(file);
    document::Document document(*file);
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path});
    host.setDocument(&document);
    host.activate();
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    REQUIRE(host.plugins()[0].loaded);
    const auto sections = host.sections("face_properties");
    REQUIRE(sections.size() == 1);
    CHECK(sections[0].id == "plugin.facestats.stats");
    CHECK(host.sections("materials").empty());
    const auto rows = [&](const UiContext& context) {
        std::vector<std::string> out;
        const bool described = host.describePanel(sections[0].id, &context, [&](const apex_ui_form& form) {
            for (size_t i = 0; i < form.control_count; ++i)
                out.emplace_back(form.controls[i].value.text.data, form.controls[i].value.text.size);
        });
        if (!described) out.emplace_back("describe failed");
        return out;
    };
    CHECK(rows({"sides", {}, {9, 4, 7}}) == std::vector<std::string>{"Faces selected: 3", "Side IDs: 4 to 9", "Solids in the map: 2"});
    CHECK(rows({"sides", {}, {5}}) == std::vector<std::string>{"Faces selected: 1", "Side IDs: 5", "Solids in the map: 2"});
    CHECK(rows({"sides", {}, {}}) == std::vector<std::string>{"Select faces to see their stats"});
    CHECK(rows({"entities", {}, {1}}) == std::vector<std::string>{"Select faces to see their stats"});
    host.setDocument(nullptr);
    CHECK(rows({"sides", {}, {5}}) == std::vector<std::string>{"Faces selected: 1", "Side IDs: 5", "No map is open"});
}

TEST_CASE("plugin activation registers only its namespace with editor registries") {
    Temp temp;
    temp.manifest("example", entryFor(REGISTRATION_PLUGIN_PATH));
    std::vector<std::string> logs, panels;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
    host.setPanelRegistrar([&](std::string id, std::string) { panels.push_back(std::move(id)); return true; });
    host.discover({temp.path});
    host.activate();
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    REQUIRE(host.plugins()[0].loaded);
    CHECK_FALSE(editor::actionRegistry().find("plugin.other.action"));
    CHECK_FALSE(editor::actionRegistry().find("plugin.example."));  // refused with APEX_ERR_INVALID (the plugin checks)
    REQUIRE(editor::actionRegistry().find("plugin.example.action"));
    REQUIRE(editor::actionRegistry().find("plugin.example.tool"));
    CHECK(panels == std::vector<std::string>{"plugin.example.panel"});
    editor::EditorSession session;
    REQUIRE(session.perform("plugin.example.action"));
    REQUIRE_FALSE(logs.empty());
    CHECK(logs.back() == "action called");
    REQUIRE(session.perform("plugin.example.tool"));
    CHECK(session.activeTool() == "plugin.example.tool");
    CHECK(logs.back() == "tool activated");
}

TEST_CASE("polygon tool recorded trace and lifecycle are deterministic") {
    Temp temp;
    temp.manifest("polygon", entryFor(POLYGON_PLUGIN_PATH));
    auto file = vmf::parse("world { id 1 classname worldspawn }");
    REQUIRE(file);
    document::Document document(*file);
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path});
    host.setDocument(&document);
    host.activate();
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    REQUIRE(host.plugins()[0].loaded);
    editor::EditorSession session;
    session.attach(document);
    REQUIRE(session.perform("plugin.polygon.tool"));
    editor::ToolView view;
    view.cursorRay = {{0, 0, 64}, {0, 0, -1}};
    editor::InputEvent event;
    event.source = 42;
    event.kind = editor::InputKind::Press;
    event.button = editor::PointerButton::Left;
    for (const auto& xy : {DVec2{0, 0}, DVec2{64, 0}, DVec2{0, 64}}) {
        view.cursorRay.origin = {xy.x, xy.y, 64};
        CHECK(session.input(event, view));
    }
    const auto first = session.activeToolObject().overlay(view, 42);
    CHECK(first.worldSegments.size() >= 3);
    CHECK_FALSE(session.activeToolObject().preview().empty());
    const auto firstGhosts = session.activeToolObject().preview().ghosts();
    REQUIRE(firstGhosts.size() == 1);
    const auto again = session.activeToolObject().overlay(view, 42);
    REQUIRE(again.worldSegments.size() == first.worldSegments.size());
    for (std::size_t i = 0; i < first.worldSegments.size(); ++i) {
        CHECK(again.worldSegments[i].a == first.worldSegments[i].a);
        CHECK(again.worldSegments[i].b == first.worldSegments[i].b);
    }
    event.kind = editor::InputKind::KeyDown;
    event.key = "Escape";
    CHECK(session.input(event, view));
    CHECK(session.activeToolObject().preview().empty());
    CHECK(session.activeToolObject().overlay(view, 42).worldSegments.empty());
    event.kind = editor::InputKind::Press;
    event.key.clear();
    for (const auto& xy : {DVec2{0, 0}, DVec2{64, 0}, DVec2{0, 64}}) {
        view.cursorRay.origin = {xy.x, xy.y, 64};
        CHECK(session.input(event, view));
    }
    const auto replay = session.activeToolObject().overlay(view, 42);
    REQUIRE(replay.worldSegments.size() == first.worldSegments.size());
    for (std::size_t i = 0; i < first.worldSegments.size(); ++i) {
        CHECK(replay.worldSegments[i].a == first.worldSegments[i].a);
        CHECK(replay.worldSegments[i].b == first.worldSegments[i].b);
    }
    CHECK_FALSE(session.activeToolObject().preview().empty());
    const auto& replayGhosts = session.activeToolObject().preview().ghosts();
    REQUIRE(replayGhosts.size() == firstGhosts.size());
    CHECK(replayGhosts[0].poly.vertices == firstGhosts[0].poly.vertices);
    CHECK(replayGhosts[0].material == firstGhosts[0].material);
    CHECK(replayGhosts[0].opacity == firstGhosts[0].opacity);
    session.forgetInputSource(42);
    CHECK(session.activeToolObject().overlay(view, 42).worldSegments.empty());
    for (const auto& xy : {DVec2{0, 0}, DVec2{64, 0}, DVec2{0, 64}}) {
        view.cursorRay.origin = {xy.x, xy.y, 64};
        CHECK(session.input(event, view));
    }
    CHECK_FALSE(session.activeToolObject().preview().empty());
    session.setActiveTool(editor::EditorSession::Tool::Select);
    REQUIRE(session.perform("plugin.polygon.tool"));
    CHECK(session.activeToolObject().overlay(view, 42).worldSegments.empty());
    host.setDocument(nullptr);
}

TEST_CASE("polygon plugin commit is one undoable edit preserving selection") {
    Temp temp;
    temp.manifest("polygon", entryFor(POLYGON_PLUGIN_PATH));
    auto file = vmf::parse("world { id 1 classname worldspawn }");
    REQUIRE(file);
    document::Document document(*file);
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path});
    host.setDocument(&document);
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    editor::EditorSession session;
    session.attach(document);
    host.setSession(&session);
    session.selection().setMode(editor::SelectionMode::Objects);
    session.selection().select(document.roots().front());
    const auto before = session.selection().toBlob();
    REQUIRE(session.perform("plugin.polygon.tool"));
    editor::ToolView view;
    view.cursorRay = {{0, 0, 64}, {0, 0, -1}};
    editor::InputEvent event;
    event.source = 42;
    event.kind = editor::InputKind::Press;
    event.button = editor::PointerButton::Left;
    for (const auto& xy : {DVec2{0, 0}, DVec2{64, 0}, DVec2{0, 64}}) {
        view.cursorRay.origin = {xy.x, xy.y, 64};
        REQUIRE(session.input(event, view));
    }
    const auto count = document.undoCount();
    REQUIRE(session.perform("plugin.polygon.commit"));
    REQUIRE(document.undoCount() == count + 1);
    REQUIRE(document.undo());
    CHECK(session.selection().toBlob() == before);
    CHECK(document.undoCount() == count);
    host.setDocument(nullptr);
    host.setSession(nullptr);
}

TEST_CASE("plugin tool state is independent between editor sessions") {
    Temp temp;
    temp.manifest("polygon", entryFor(POLYGON_PLUGIN_PATH));
    auto file = vmf::parse("world { id 1 classname worldspawn }");
    REQUIRE(file);
    document::Document document(*file);
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path});
    host.setDocument(&document);
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    editor::EditorSession first, second;
    first.attach(document); second.attach(document);
    REQUIRE(first.perform("plugin.polygon.tool"));
    REQUIRE(second.perform("plugin.polygon.tool"));
    editor::ToolView view;
    view.cursorRay = {{0, 0, 64}, {0, 0, -1}};
    editor::InputEvent event;
    event.source = 42; event.kind = editor::InputKind::Press; event.button = editor::PointerButton::Left;
    for (const auto& xy : {DVec2{0, 0}, DVec2{64, 0}, DVec2{0, 64}}) {
        view.cursorRay.origin = {xy.x, xy.y, 64};
        REQUIRE(first.input(event, view));
    }
    const auto before = first.activeToolObject().overlay(view, 42);
    REQUIRE(before.worldSegments.size() == 3);
    view.cursorRay.origin = {200, 200, 64};
    REQUIRE(second.input(event, view));
    CHECK(first.activeToolObject().overlay(view, 42).worldSegments.size() == before.worldSegments.size());
    CHECK(second.activeToolObject().overlay(view, 42).worldSegments.empty());
    host.setDocument(nullptr);
}

TEST_CASE("plugin edit failure rolls back and deferred notification makes a separate step") {
    Temp temp;
    temp.manifest("writes", entryFor(WRITE_PLUGIN_PATH));
    auto file = vmf::parse("world { id 1 classname worldspawn }");
    REQUIRE(file);
    document::Document document(*file);
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
    host.discover({temp.path});
    host.setDocument(&document);
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    editor::EditorSession session;
    session.attach(document);
    host.setSession(&session);
    REQUIRE(session.perform("plugin.writes.fail"));
    CHECK(document.undoCount() == 0);
    CHECK(document.get(*document.handle(document.roots().front()))->block->find("before_failure") == nullptr);
    REQUIRE_FALSE(logs.empty());
    CHECK(logs.back() == "failed as expected");
    REQUIRE(session.perform("plugin.writes.selection"));
    CHECK(logs.back() == "selection ok");
    std::vector<std::string> observerOrder;
    const auto token = document.observe([&](const document::ChangeSet&) { observerOrder.push_back("observer"); logs.push_back("observer"); });
    REQUIRE(session.perform("plugin.writes.edit"));
    CHECK(document.undoCount() == 2);
    CHECK(observerOrder == std::vector<std::string>{"observer", "observer"});
    CHECK(logs == std::vector<std::string>{"failed as expected", "selection ok", "notice", "observer", "deferred", "notice", "observer"});
    REQUIRE(document.undo());
    CHECK(logs[logs.size() - 2] == "undo notice");
    document.unobserve(token);
    host.setSession(nullptr);
    host.setDocument(nullptr);
}

TEST_CASE("plugin solid rebuild retains overlay face reference") {
    Temp temp;
    temp.manifest("writes", entryFor(WRITE_PLUGIN_PATH));
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    const auto world = *document.handle(document.roots().front());
    const auto solid = editor_test::addBox(document, {0, 0, 0}, {64, 64, 64}, world);
    const auto geometry = geometry::solidGeometry(*document.get(solid)->block);
    REQUIRE(geometry);
    int top = 0;
    for (const auto& face : geometry->poly.faces) if (face.plane.normal.z > 0.999) top = static_cast<int>(face.sideId);
    REQUIRE(top != 0);
    const auto overlay = editor_test::addBlock(document, "entity { id 0 classname info_overlay sides \"" + std::to_string(top) + "\" }");
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path}); host.setDocument(&document); host.activate();
    REQUIRE(host.plugins()[0].loaded);
    editor::EditorSession session;
    session.attach(document); host.setSession(&session);
    const auto count = document.undoCount();
    REQUIRE(session.perform("plugin.writes.rebuild"));
    CHECK(document.undoCount() == count + 1);
    REQUIRE(document.side(top));
    CHECK(document.get(overlay)->block->find("sides")->decodedValue() == std::to_string(top));
    host.setSession(nullptr); host.setDocument(nullptr);
}

TEST_CASE("P1 registrations and existing adapters survive host destruction safely") {
    Temp temp;
    temp.manifest("example", entryFor(REGISTRATION_PLUGIN_PATH));
    std::vector<std::string> logs;
    editor::EditorSession session;
    std::unique_ptr<editor::Tool> adapter;
    editor::ToolRegistry::Factory factory;
    {
        Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
        host.setPanelRegistrar([](std::string, std::string) { return true; });
        host.discover({temp.path}); host.activate();
        REQUIRE(host.plugins()[0].loaded);
        for (const auto& entry : editor::toolRegistry().entries()) if (entry.id == "plugin.example.tool") factory = entry.factory;
        REQUIRE(factory);
        adapter = factory(session);
        REQUIRE(adapter);
    }
    const auto count = logs.size();
    adapter->activate(); adapter->deactivate(); adapter.reset();
    CHECK(session.perform("plugin.example.action"));
    auto inert = factory(session);
    REQUIRE(inert);
    inert->activate(); inert->deactivate();
    CHECK(logs.size() == count);
}

TEST_CASE("P1 plugin state callbacks publish status except pane forget and destruction") {
    editor::EditorSession session;
    apex_tool_callbacks callbacks{};
    callbacks.input = [](void*, const apex_tool_event*, const apex_tool_view*) { return 1; };
    callbacks.set_parameter = [](void*, apex_str, double) { return 1; };
    callbacks.perform = [](void*, apex_str) { return 1; };
    callbacks.cancel = [](void*) {};
    int notices = 0;
    session.observe([&] { ++notices; });
    int beforeDestruction = 0;
    {
        PluginTool tool(session, "test", nullptr, callbacks, [](const char*, const std::function<void()>& fn) { fn(); });
        auto check = [&](auto fn) { const auto before = notices; fn(); CHECK(notices > before); };
        check([&] { tool.input({}, {}); });
        check([&] { tool.setParameter("height", 32); });
        check([&] { tool.perform("commit"); });
        check([&] { tool.cancel(); });
        check([&] { tool.documentChanged(); });
        const auto before = notices;
        tool.forget(0);
        CHECK(notices == before);
        beforeDestruction = notices;
    }
    CHECK(notices == beforeDestruction);
}

TEST_CASE("P1 view exceptions return inside the host ABI boundary") {
    editor::EditorSession session;
    apex_tool_callbacks callbacks{};
    apex_status status = APEX_OK;
    callbacks.input = [](void* user, const apex_tool_event*, const apex_tool_view* view) {
        apex_plane planes[4];
        *static_cast<apex_status*>(user) = view->rect_frustum(view->context, 0, 0, 1, 1, planes);
        return 1;
    };
    std::string error;
    PluginTool tool(session, "test", &status, callbacks, [](const char*, const std::function<void()>& fn) { fn(); },
                    [&](const char* message) { error = message; });
    editor::ToolView view;
    view.rectFrustum = [](DVec2, DVec2) -> std::array<Plane, 4> { throw std::runtime_error("frustum failure"); };
    CHECK_NOTHROW(tool.input({}, view));
    CHECK(status == APEX_ERR_INTERNAL);
    CHECK(error == "frustum failure");
}

TEST_CASE("P1 registration exceptions are contained and partial entries rolled back") {
    Temp temp;
    temp.manifest("exceptions", entryFor(EXCEPTION_PLUGIN_PATH));
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    Host host(temp.path / "state", temp.path / "breadcrumb", [](std::string_view) { throw std::runtime_error("logger failure"); });
    host.setSession(&session); host.setDocument(&document);
    host.discover({temp.path}); host.activate();
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    CHECK(host.plugins()[0].loaded);
    CHECK(document.undoCount() == 0);
    CHECK_FALSE(document.get(*document.handle(1))->block->find("before_failure"));
    CHECK(editor::actionRegistry().find("plugin.exceptions.basic"));
    CHECK(editor::actionRegistry().find("plugin.exceptions.suggest"));
}

TEST_CASE("P1 extended adapters and document observers retire before library unload") {
    Temp temp;
    temp.manifest("polygon", entryFor(POLYGON_PLUGIN_PATH));
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    {
        Host host(temp.path / "state", temp.path / "breadcrumb");
        host.discover({temp.path}); host.setDocument(&document); host.setSession(&session); host.activate();
        REQUIRE(host.plugins()[0].loaded);
        // Construct after registration so this adapter owns a plugin instance.
        for (const auto& entry : editor::toolRegistry().entries()) if (entry.id == "plugin.polygon.tool")
            REQUIRE(session.addTool(entry.factory(session)));
        REQUIRE(session.perform("plugin.polygon.tool"));
    }
    auto& tool = session.activeToolObject();
    CHECK_FALSE(tool.input({}, {}));
    CHECK(tool.preview().empty());
    CHECK(tool.overlay({}, 0).worldSegments.empty());
    tool.cancel(); tool.documentChanged(); tool.forget(0); tool.deactivate();
    // The old document observer must not call the destroyed host.
    session.edit("After host destruction", [&](document::Transaction& tx) {
        tx.setKeyValue(*document.handle(document.roots().front()), "message", "safe");
    });
    editor::EditorSession later;
    REQUIRE(later.perform("plugin.polygon.tool"));
    CHECK_FALSE(later.input({}, {}));
}

TEST_CASE("overlays, helpers and problems from two plugins") {
    Temp temp;
    temp.manifest("wrap", entryFor(OVERLAY_PLUGIN_PATH));
    temp.manifest("other", entryFor(OVERLAY_OTHER_PLUGIN_PATH));
    document::Document document(editor_test::parsed(
        "world { id 1 classname worldspawn } entity { id 7 classname wrapent origin \"32 0 0\" } "
        "entity { id 8 classname wrapent origin \"0 64 0\" }"));
    editor::EditorSession session;
    session.attach(document);
    assets::EntityClassTable classes;
    assets::EntityClass cls; cls.name = "wrapent"; cls.type = "PointClass";
    // widget draws; thrower throws after drawing; sphere is a host helper and must not reach plugins.
    cls.helpers = {{"wrapwidget", "16"}, {"otherwidget", ""}, {"wrapthrower", ""}, {"sphere", "radius"}};
    classes.classes["wrapent"] = std::move(cls);
    session.setEntityClasses(std::move(classes));
    std::vector<std::string> logs;
    editor::ProblemRegistry registry;
    {
        Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
        host.useProblemRegistry(registry);
        host.discover({temp.path}); host.setDocument(&document); host.setSession(&session);
        host.activate();
        REQUIRE(host.plugins().size() == 2);
        for (const auto& plugin : host.plugins()) { CAPTURE(plugin.error); REQUIRE(plugin.loaded); }
        const auto count = [&](const std::string& line) { return std::ranges::count(logs, line); };
        const auto invalid = std::to_string(APEX_ERR_INVALID);
        CHECK(count("declare_dup=" + invalid) == 2);
        CHECK(count("declare_outside=" + invalid) == 2);
        CHECK(count("helper_builtin=" + invalid) == 2);
        CHECK(count("helper_dup=" + invalid) == 2);
        CHECK(count("problem_outside=" + invalid) == 2);

        // Overlays: declared in activate with the plugin's data, per-plugin and namespaced.
        REQUIRE(host.overlays().size() == 4);
        const auto find = [&](std::string_view id) -> const OverlayData* {
            for (const auto& overlay : host.overlays()) if (overlay.id == id) return &overlay;
            return nullptr;
        };
        const auto* main = find("plugin.wrap.main");
        REQUIRE(main);
        CHECK(main->name == "Wrap overlay");
        CHECK(main->segments.size() == 1 + 12);   // the line plus the ghost cube's edges
        CHECK(main->points.size() == 1);
        CHECK(main->labels.size() == 1);
        CHECK(main->triangles.size() == 12);      // 6 quads as 2 triangles each
        CHECK(main->tints.size() == 1);
        CHECK(main->tints.at(5).r == doctest::Approx(1.0f));
        CHECK(main->tints.at(5).a == doctest::Approx(0.5f));
        CHECK(find("plugin.wrap.other")->segments.empty());

        // Callbacks: set works, invalid lists are rejected whole, ownership is enforced.
        const auto revision = host.overlayRevision();
        logs.clear();
        REQUIRE(session.perform("plugin.wrap.act"));
        CHECK(host.overlayRevision() > revision);
        const auto state = [&](const char*) { return std::to_string(APEX_ERR_STATE); };
        CHECK(std::ranges::count(logs, "set=0") == 1);
        CHECK(std::ranges::count(logs, "set_nan=" + invalid) == 1);
        CHECK(std::ranges::count(logs, "set_unknown=" + std::to_string(APEX_ERR_NOT_FOUND)) == 1);
        CHECK(std::ranges::count(logs, "steal=" + invalid) == 1);
        CHECK(std::ranges::count(logs, "declare_late=" + state("")) == 1);
        CHECK(std::ranges::count(logs, "helper_late=" + state("")) == 1);
        CHECK(find("plugin.wrap.main")->points.size() == 1); // the NaN list left it unchanged
        REQUIRE(session.perform("plugin.other.clear"));
        CHECK(find("plugin.wrap.main")->points.size() == 1); // other's clear touched only other's overlay
        REQUIRE(session.perform("plugin.wrap.clear"));
        CHECK(find("plugin.wrap.main")->points.empty());
        CHECK(find("plugin.wrap.main")->name == "Wrap overlay"); // still declared, just empty
        REQUIRE(session.perform("plugin.wrap.act"));
        CHECK_FALSE(find("plugin.wrap.main")->segments.empty());
        // A throwing callback is contained.
        CHECK(session.perform("plugin.wrap.throw"));
        // Overlay data belongs to a document: it is dropped when the document closes.
        host.setDocument(nullptr);
        CHECK(find("plugin.wrap.main")->segments.empty());
        host.setDocument(&document);

        // Helpers: host helpers never reach plugins; unknown names draw per entity; failures roll back.
        REQUIRE(host.hasHelpers());
        const auto& drawing = host.helperDrawing();
        CHECK(drawing.segments.size() == 4);      // wrapwidget and otherwidget on two entities each
        CHECK(drawing.labels.size() == 4);
        const auto atX32 = std::ranges::count_if(drawing.segments, [](const OverlaySegment& s) { return s.a.x == 32.0 && s.b.x == 48.0; });
        CHECK(atX32 == 2);
        const auto* cached = &host.helperDrawing();
        CHECK(cached == &drawing);
        // Hidden entities draw nothing.
        REQUIRE(session.setNodeHidden({document.roots()[1]}, true));
        CHECK(host.helperDrawing().segments.size() == 2);
        REQUIRE(session.unhideAll());
        CHECK(host.helperDrawing().segments.size() == 4);

        // Problems: grouped by plugin source, objects validated, a throwing check reports itself.
        const auto problems = registry.run(editor::ProblemContext{document});
        const auto text = [&](std::string_view needle, std::string_view source) {
            return std::ranges::count_if(problems, [&](const editor::Problem& p) {
                return p.text.find(needle) != std::string::npos && p.source == source && p.kind == editor::ProblemKind::Plugin;
            });
        };
        CHECK(text("roots=3", "wrap") == 1);
        CHECK(text("roots=3", "other") == 1);
        CHECK(text("first root", "wrap") == 1);
        CHECK(text("failed", "wrap") == 1);       // the broken check
        CHECK(text("half a result", "wrap") == 0); // partial results are dropped
        const auto first = std::ranges::find_if(problems, [](const editor::Problem& p) { return p.text == "first root" && p.source == "wrap"; });
        REQUIRE(first != problems.end());
        CHECK(first->fixId == "select");
        CHECK(first->objects == std::vector<document::NodeId>{document.roots()[0]});
        CHECK(first->severity == editor::ProblemSeverity::Warning);
        CHECK(std::ranges::count(logs, "bad_severity=" + invalid) >= 1);
        CHECK(std::ranges::count(logs, "unknown_object=" + std::to_string(APEX_ERR_NOT_FOUND)) >= 1);
        const auto groups = editor::groupProblems(problems);
        REQUIRE(groups.size() == 2);
        CHECK(((groups[0].source == "wrap" && groups[1].source == "other") || (groups[0].source == "other" && groups[1].source == "wrap")));
        std::size_t grouped = 0;
        for (const auto& group : groups) grouped += group.indices.size();
        CHECK(grouped == problems.size());
    }
    // The host is gone: its registered check must be inert rather than call freed state.
    CHECK_NOTHROW(registry.run(editor::ProblemContext{document}));
    CHECK(registry.run(editor::ProblemContext{document}).empty());
}

TEST_CASE("a plugin cannot register problem checks after the registry freezes") {
    Temp temp;
    temp.manifest("wrap", entryFor(OVERLAY_PLUGIN_PATH));
    editor::ProblemRegistry registry;
    registry.freeze();
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.useProblemRegistry(registry);
    host.discover({temp.path}); host.activate();
    REQUIRE(host.plugins().size() == 1);
    CHECK_FALSE(host.plugins()[0].loaded);
    CHECK(host.overlays().empty()); // a failed activation leaves no overlays or helpers behind
    CHECK_FALSE(host.hasHelpers());
}

namespace {
std::size_t worldChildren(document::Document& document) {
    for (const auto root : document.roots()) {
        const auto handle = document.handle(root);
        const auto* node = handle ? document.get(*handle) : nullptr;
        if (node && node->block && node->block->key() == "world") return node->children.size();
    }
    return 0;
}
std::string threadHash() { return std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())); }
}

TEST_CASE("obj-import imports a small OBJ as one undo step") {
    Temp temp;
    temp.manifest("objimport", entryFor(OBJ_IMPORT_PLUGIN_PATH));
    // A cube (with v/vt/vn references and quad faces) and a triangular wedge, as two groups.
    std::ofstream(temp.path / "parts.obj") <<
        "o cube\nusemtl DEV/DEV_MEASUREGENERIC01\n"
        "v 0 0 0\nv 64 0 0\nv 64 64 0\nv 0 64 0\nv 0 0 64\nv 64 0 64\nv 64 64 64\nv 0 64 64\n"
        "f 1/1/1 4/1/1 3/1/1 2/1/1\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n"
        "o wedge\nv 100 0 0\nv 164 0 0\nv 100 64 0\nv 100 0 64\nv 164 0 64\nv 100 64 64\n"
        "f -6 -4 -5\nf -3 -2 -1\nf -6 -5 -2 -3\nf -5 -4 -1 -2\nf -4 -6 -3 -1\n";
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
    host.discover({temp.path}); host.setDocument(&document); host.setSession(&session);
    host.activate();
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    REQUIRE(host.plugins()[0].loaded);
    REQUIRE(host.importers().size() == 1);
    CHECK(host.importers()[0].id == "plugin.objimport.obj");
    REQUIRE(host.importerFor(temp.path / "SHAPE.OBJ"));
    CHECK_FALSE(host.importerFor(temp.path / "shape.fbx"));

    const auto before = document.undoCount();
    const auto solids = worldChildren(document);
    const auto result = host.importFile("plugin.objimport.obj", temp.path / "parts.obj");
    CAPTURE(result.message);
    REQUIRE(result.ok);
    CHECK(worldChildren(document) == solids + 2);
    CHECK(document.undoCount() == before + 1);
    REQUIRE(document.undo());
    CHECK(worldChildren(document) == solids);
    CHECK(document.undoCount() == before);
    host.setDocument(nullptr); host.setSession(nullptr);
}

TEST_CASE("a failing import rolls every solid back") {
    Temp temp;
    temp.manifest("objimport", entryFor(OBJ_IMPORT_PLUGIN_PATH));
    // The first group is a good cube; the second is open (one face), so the whole import must fail.
    std::ofstream(temp.path / "bad.obj") <<
        "o cube\nv 0 0 0\nv 64 0 0\nv 64 64 0\nv 0 64 0\nv 0 0 64\nv 64 0 64\nv 64 64 64\nv 0 64 64\n"
        "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n"
        "o open\nv 200 0 0\nv 264 0 0\nv 264 64 0\nf 9 10 11\n";
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path}); host.setDocument(&document); host.setSession(&session);
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    const auto before = document.undoCount();
    const auto solids = worldChildren(document);
    const auto result = host.importFile("plugin.objimport.obj", temp.path / "bad.obj");
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("open") != std::string::npos);
    CHECK(worldChildren(document) == solids);
    CHECK(document.undoCount() == before);
    CHECK_FALSE(host.importFile("plugin.objimport.obj", temp.path / "missing.obj").ok);
    CHECK_FALSE(host.importFile("plugin.nope.obj", temp.path / "bad.obj").ok);
    host.setDocument(nullptr); host.setSession(nullptr);
}

TEST_CASE("a job's completion runs on the main thread") {
    Temp temp;
    temp.manifest("async", entryFor(ASYNC_PLUGIN_PATH));
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
    host.discover({temp.path}); host.setDocument(&document); host.setSession(&session);
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    const auto has = [&](std::string_view prefix) {
        return std::ranges::find_if(logs, [&](const std::string& l) { return l.starts_with(prefix); }) != logs.end();
    };
    const auto wait = [&](std::string_view prefix) {
        for (int i = 0; i < 400 && !has(prefix); ++i) { host.drainJobs(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        return has(prefix);
    };

    REQUIRE(session.perform("plugin.async.job"));
    // Completion is not delivered until the editor thread drains.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(has("done:plain"));
    REQUIRE(wait("done:plain"));
    const auto line = *std::ranges::find_if(logs, [](const std::string& l) { return l.starts_with("done:plain"); });
    CAPTURE(line);
    CHECK(line.find(":status=0:") != std::string::npos);
    CHECK(line.find(":main=" + threadHash()) != std::string::npos);   // delivered on this thread
    CHECK(line.find(":work=" + threadHash() + ":") == std::string::npos);   // ran on another
    CHECK(host.pendingJobs() == 0);

    // An exception in work arrives as an internal error; one in the completion is contained.
    REQUIRE(session.perform("plugin.async.failing"));
    REQUIRE(wait("done:failing"));
    CHECK(std::ranges::any_of(logs, [](const std::string& l) { return l.starts_with("done:failing:status=" + std::to_string(APEX_ERR_INTERNAL)); }));
    REQUIRE(session.perform("plugin.async.throwing"));
    REQUIRE(wait("done:throwing"));

    // Cancelling before delivery suppresses the completion.
    REQUIRE(session.perform("plugin.async.cancel"));
    CHECK(std::ranges::count(logs, "cancel=" + std::to_string(APEX_OK)) == 1);
    CHECK(std::ranges::count(logs, "cancel_again=" + std::to_string(APEX_ERR_NOT_FOUND)) == 1);
    for (int i = 0; i < 40; ++i) { host.drainJobs(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    CHECK_FALSE(has("done:cancelled"));
    host.setDocument(nullptr); host.setSession(nullptr);
}

TEST_CASE("jobs still running at host shutdown are joined, not delivered") {
    Temp temp;
    temp.manifest("async", entryFor(ASYNC_PLUGIN_PATH));
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    std::vector<std::string> logs;
    {
        Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
        host.discover({temp.path}); host.setDocument(&document); host.setSession(&session);
        host.activate();
        REQUIRE(host.plugins()[0].loaded);
        REQUIRE(session.perform("plugin.async.cancel"));
        REQUIRE(session.perform("plugin.async.job"));
        host.setDocument(nullptr); host.setSession(nullptr);
    }
    CHECK(std::ranges::none_of(logs, [](const std::string& l) { return l.starts_with("done:"); }));
}

TEST_CASE("a watched file's change is delivered once per save") {
    Temp temp;
    temp.manifest("async", entryFor(ASYNC_PLUGIN_PATH));
    const auto file = temp.path / "async" / "watched.txt";
    std::ofstream(file) << "one";
    setenv("APEX_TEST_WATCH", file.string().c_str(), 1);
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
    host.discover({temp.path}); host.setDocument(&document); host.setSession(&session);
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    host.setWatchDebounce(std::chrono::milliseconds(200));
    REQUIRE(session.perform("plugin.async.watch"));
    CHECK(std::ranges::count(logs, "watch=" + std::to_string(APEX_OK)) == 1);
    CHECK(host.hasWatches());
    const auto changes = [&] {
        return std::ranges::count_if(logs, [](const std::string& l) { return l.starts_with("changed:"); });
    };
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    const auto ms = [](int n) { return std::chrono::milliseconds(n); };
    const auto write = [&](const std::string& text, int seconds) {
        std::ofstream(file, std::ios::trunc) << text;
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(seconds));
    };

    host.pollWatches(now);
    CHECK(changes() == 0);                          // nothing changed since the watch began

    // One save written in three pieces within the debounce window is one delivery.
    write("two", 1);   host.pollWatches(now += ms(10));
    write("three!", 2); host.pollWatches(now += ms(50));
    write("four!!!", 3); host.pollWatches(now += ms(50));
    CHECK(changes() == 0);                          // still inside the burst
    host.pollWatches(now += ms(150));
    CHECK(changes() == 0);                          // quiet for 150 ms of the 200 ms
    host.pollWatches(now += ms(100));
    REQUIRE(changes() == 1);
    CHECK(logs.back().find(":main=" + threadHash()) != std::string::npos);
    host.pollWatches(now += ms(500));
    host.pollWatches(now += ms(500));
    CHECK(changes() == 1);                          // quiet afterwards: no repeat

    // A second, separate save is a second delivery.
    write("five", 10);
    host.pollWatches(now += ms(10));
    host.pollWatches(now += ms(300));
    CHECK(changes() == 2);

    // Deleting the file is a change too; unwatch stops deliveries.
    std::filesystem::remove(file);
    host.pollWatches(now += ms(10));
    host.pollWatches(now += ms(300));
    CHECK(changes() == 3);
    REQUIRE(session.perform("plugin.async.unwatch"));
    CHECK(std::ranges::count(logs, "unwatch=" + std::to_string(APEX_OK)) == 1);
    std::ofstream(file) << "back";
    host.pollWatches(now += ms(10));
    host.pollWatches(now += ms(300));
    CHECK(changes() == 3);
    CHECK_FALSE(host.hasWatches());
    unsetenv("APEX_TEST_WATCH");
    host.setDocument(nullptr); host.setSession(nullptr);
}

TEST_CASE("watch refuses paths outside the plugin folder and survives a throwing callback") {
    Temp temp;
    temp.manifest("async", entryFor(ASYNC_PLUGIN_PATH));
    const auto boom = temp.path / "async" / "boom.txt";
    std::ofstream(boom) << "x";
    setenv("APEX_TEST_WATCH", boom.string().c_str(), 1);
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
    host.discover({temp.path}); host.setDocument(&document); host.setSession(&session);
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    REQUIRE(session.perform("plugin.async.watch_bad"));
    const auto invalid = std::to_string(APEX_ERR_INVALID);
    CHECK(std::ranges::count(logs, "watch_relative=" + invalid) == 1);
    CHECK(std::ranges::count(logs, "watch_outside=" + invalid) == 1);
    REQUIRE(session.perform("plugin.async.watch"));
    auto now = std::chrono::steady_clock::now();
    std::ofstream(boom, std::ios::trunc) << "changed contents";
    host.pollWatches(now);
    CHECK(host.pollWatches(now + std::chrono::milliseconds(300)) == 1);   // callback threw; host carried on
    unsetenv("APEX_TEST_WATCH");
    host.setDocument(nullptr); host.setSession(nullptr);
}

// ---- 0.9.3: edit operations and change notices -------------------------------------------------------------
namespace {
std::string noticeText(const document::ChangeSet& changes, std::uint64_t revision) {
    std::string text = "notice rev=" + std::to_string(revision) + " u=" + (changes.undoOrRedo ? "1" : "0") + " c=";
    for (const auto id : changes.createdIds) text += std::to_string(id) + ",";
    text += " m=";
    for (const auto& item : changes.modifiedIds) text += std::to_string(item.id) + ":" + std::to_string(static_cast<unsigned>(item.flags)) + ",";
    text += " d=";
    for (const auto id : changes.destroyedIds) text += std::to_string(id) + ",";
    text += " v=";
    for (const auto side : changes.vanishedSides) text += std::to_string(side) + ",";
    return text;
}
std::string nodeText(const kv1::Node* node) {
    if (!node) return "<none>";
    const auto value = node->decodedValue();
    return value ? *value : "<bad>";
}
struct Notice {
    std::string text;
    std::uint64_t revision = 0;
    bool undoOrRedo = false;
    std::vector<document::NodeId> created, destroyed;
    std::vector<std::pair<document::NodeId, unsigned>> modified;
    std::vector<int> vanished;
};
// A world with one 64-unit box and an entity whose `sides` key lists the box's six sides (+X -X +Y -Y +Z -Z),
// driven by tests/plugin_host/edit_ops_plugin.c.
struct EditFixture {
    Temp temp;
    document::Document document{editor_test::parsed("world { id 1 classname worldspawn }")};
    std::vector<std::string> logs;
    std::vector<Notice> notices;      // the changes the document reported, with the revision the host saw
    std::vector<std::string> plugin;  // the notice lines the plugin logged
    document::NodeHandle solid, entity;
    std::array<int, 6> sides{};
    Host host;
    editor::EditorSession session;
    std::size_t token = 0;
    EditFixture() : host(temp.path / "state", temp.path / "breadcrumb", [this](std::string_view text) {
        logs.emplace_back(text);
        if (text.starts_with("notice ")) plugin.emplace_back(text);
    }) {
        temp.manifest("editops", entryFor(EDIT_OPS_PLUGIN_PATH));
        const auto world = *document.handle(document.roots().front());
        solid = editor_test::addBox(document, {0, 0, 0}, {64, 64, 64}, world);
        const auto geometry = geometry::solidGeometry(*document.get(solid)->block);
        REQUIRE(geometry);
        const DVec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        std::string list;
        for (int i = 0; i < 6; ++i) {
            for (const auto& face : geometry->poly.faces)
                if (glm::dot(face.plane.normal, normals[i]) > 0.999) sides[static_cast<size_t>(i)] = static_cast<int>(face.sideId);
            REQUIRE(sides[static_cast<size_t>(i)] != 0);
            list += (i ? " " : "") + std::to_string(sides[static_cast<size_t>(i)]);
        }
        entity = editor_test::addBlock(document, "entity { id 0 classname info_overlay targetname \"old\" sides \"" + list + "\" }");
        host.discover({temp.path}); host.setDocument(&document); host.activate();
        REQUIRE(host.plugins()[0].loaded);
        session.attach(document); host.setSession(&session);
        token = document.observe([this](const document::ChangeSet& changes) {
            Notice n;
            n.revision = document.historyState(); n.undoOrRedo = changes.undoOrRedo;
            n.text = noticeText(changes, n.revision);
            n.created = changes.createdIds; n.destroyed = changes.destroyedIds;
            for (const auto& m : changes.modifiedIds) n.modified.emplace_back(m.id, static_cast<unsigned>(m.flags));
            for (const auto s : changes.vanishedSides) n.vanished.push_back(s);
            notices.push_back(std::move(n));
        });
    }
    ~EditFixture() { document.unobserve(token); host.setSession(nullptr); host.setDocument(nullptr); }
    const kv1::Node* childBlock(std::string_view name) const {   // connections and editor are child nodes
        for (const auto child : document.get(entity)->children) {
            const auto* node = document.get(*document.handle(child))->block.get();
            if (node->key() == name) return node;
        }
        return nullptr;
    }
    const kv1::Node* connections() const { return childBlock("connections"); }
    document::NodeId solidId() const { return document.get(solid)->id; }
    document::NodeId entityId() const { return document.get(entity)->id; }
    // Runs one plugin action; every notice the plugin saw must equal the document's, field for field.
    std::string run(const std::string& action) {
        logs.clear(); plugin.clear(); notices.clear();
        REQUIRE(session.perform("plugin.editops." + action));
        compare();
        for (const auto& line : logs) if (line.starts_with(action + "=")) return line.substr(action.size() + 1);
        FAIL("plugin did not report " << action);
        return {};
    }
    void compare() {
        REQUIRE(plugin.size() == notices.size());
        for (size_t i = 0; i < plugin.size(); ++i) CHECK(plugin[i] == notices[i].text);
    }
    std::string key(document::NodeHandle h, std::string_view name) const {
        return nodeText(document.get(h)->block->find(name));
    }
    const kv1::Node* sideBlock(int id) const {
        for (const auto& child : document.get(solid)->block->children()) {
            if (child.kind() != kv1::NodeKind::Block || child.key() != "side") continue;
            if (const auto* sideId = child.find("id"); sideId && nodeText(sideId) == std::to_string(id)) return &child;
        }
        return nullptr;
    }
    void selectSolid() {
        session.selection().setMode(editor::SelectionMode::Solids);
        session.selection().select(solidId());
    }
    bool solidSelected() const {
        return session.selection().mode() == editor::SelectionMode::Solids && session.selection().ordered().size() == 1 &&
               session.selection().contains(solidId());
    }
    double maxX() const {
        const auto geometry = geometry::solidGeometry(*document.get(solid)->block);
        double result = -1e9;
        for (const auto& v : geometry->poly.vertices) result = std::max(result, v.x);
        return result;
    }
};
const std::string kInvalid = std::to_string(APEX_ERR_INVALID);
constexpr unsigned kGeometry = 1, kMaterial = 2, kKeyValues = 4, kVisibility = 8, kHierarchy = 16;
}

TEST_CASE("edit op set_solid_geometry writes geometry and UVs in one undo step") {
    EditFixture f;
    f.selectSolid();
    CHECK(f.run("set_material") == "0");
    const auto before = f.document.undoCount();
    const auto revision = f.document.historyState();
    CHECK(f.run("set_geometry") == "0");
    CHECK(f.document.undoCount() == before + 1);
    CHECK(f.maxX() == doctest::Approx(128.0));
    const auto* top = f.sideBlock(f.sides[4]);
    REQUIRE(top);
    CHECK(nodeText(top->find("uaxis")).find("0.5") != std::string::npos);  // scale 0.5, shift 3.5
    CHECK(nodeText(top->find("uaxis")).find("3.5") != std::string::npos);
    CHECK(nodeText(top->find("material")) == "DEV/TEST");   // the side keeps its material
    REQUIRE(f.notices.size() == 1);
    CHECK(f.notices[0].revision > revision);
    CHECK(f.notices[0].revision == f.document.historyState());
    CHECK_FALSE(f.notices[0].undoOrRedo);
    REQUIRE(f.notices[0].modified.size() >= 1);
    const auto it = std::ranges::find_if(f.notices[0].modified, [&](const auto& m) { return m.first == f.solidId(); });
    REQUIRE(it != f.notices[0].modified.end());
    CHECK((it->second & kGeometry) != 0);
    CHECK(f.notices[0].created.empty()); CHECK(f.notices[0].destroyed.empty()); CHECK(f.notices[0].vanished.empty());
    CHECK(f.solidSelected());
    CHECK(f.key(f.entity, "sides") == f.key(f.entity, "sides"));
    // Undo is one step and reports undo_or_redo with a new revision.
    f.logs.clear(); f.plugin.clear(); f.notices.clear();
    f.session.selection().clear();                      // undo must bring the edit's selection back
    REQUIRE_FALSE(f.solidSelected());
    REQUIRE(f.document.undo());
    REQUIRE(editor::restoreFromUndo(f.document, f.session.selection()));   // what the app does after an undo
    f.compare();
    REQUIRE(f.notices.size() == 1);
    CHECK(f.notices[0].undoOrRedo);
    CHECK(f.maxX() == doctest::Approx(64.0));
    CHECK(f.solidSelected());
    f.notices.clear(); f.plugin.clear();
    f.session.selection().clear();
    REQUIRE(f.document.redo());
    REQUIRE(editor::restoreFromUndo(f.document, f.session.selection()));
    f.compare();
    CHECK(f.notices.back().undoOrRedo);
    CHECK(f.maxX() == doctest::Approx(128.0));
    CHECK(f.solidSelected());
}

TEST_CASE("edit op failures roll the whole batch back") {
    EditFixture f;
    f.selectSolid();
    const auto undoCount = f.document.undoCount();
    const auto revision = f.document.historyState();
    for (const char* name : {"set_geometry_bad", "set_displacement_bad", "set_connections_bad", "rebuild_matched_bad", "create_block_bad", "editor_bad"}) {
        CAPTURE(name);
        CHECK(f.run(name) == kInvalid);
        CHECK(f.document.undoCount() == undoCount);
        CHECK(f.document.historyState() == revision);
        CHECK(f.key(f.entity, "marker") == "<none>");   // the batch's first operation was rolled back too
        CHECK(f.notices.empty());
        CHECK(f.solidSelected());
    }
    CHECK(f.maxX() == doctest::Approx(64.0));
}

TEST_CASE("edit op set_displacement validates and writes a dispinfo") {
    EditFixture f;
    f.selectSolid();
    REQUIRE(f.sideBlock(f.sides[4]));
    CHECK(f.sideBlock(f.sides[4])->find("dispinfo") == nullptr);
    const auto before = f.document.undoCount();
    CHECK(f.run("set_displacement") == "0");
    CHECK(f.document.undoCount() == before + 1);
    const auto* disp = f.sideBlock(f.sides[4])->find("dispinfo");
    REQUIRE(disp);
    CHECK(nodeText(disp->find("power")) == "2");
    REQUIRE(f.notices.size() == 1);
    const auto it = std::ranges::find_if(f.notices[0].modified, [&](const auto& m) { return m.first == f.solidId(); });
    REQUIRE(it != f.notices[0].modified.end());
    CHECK((it->second & kGeometry) != 0);
    CHECK(f.solidSelected());
    REQUIRE(f.document.undo());
    CHECK(f.sideBlock(f.sides[4])->find("dispinfo") == nullptr);
}

TEST_CASE("edit op set_connections replaces and clears an entity's outputs") {
    EditFixture f;
    const auto undoBefore = f.document.undoCount();
    CHECK(f.run("set_connections") == "0");
    CHECK(f.document.undoCount() == undoBefore + 1);
    const auto* connections = f.connections();
    REQUIRE(connections);
    REQUIRE(connections->children().size() == 2);
    CHECK(connections->children()[0].key() == "OnTrigger");
    const auto first = nodeText(&connections->children()[0]);
    CHECK(first.find("door") != std::string::npos);
    CHECK(first.find("Open") != std::string::npos);
    REQUIRE(f.notices.size() == 1);                  // the block is a child node of the entity
    CHECK(f.notices[0].created.size() == 1);
    CHECK(f.run("set_connections") == "0");          // replacing keeps the node and reports a key-value change
    REQUIRE(f.notices.size() == 1);
    CHECK(f.notices[0].created.empty());
    REQUIRE(f.notices[0].modified.size() == 1);
    CHECK((f.notices[0].modified[0].second & kKeyValues) != 0);
    CHECK(f.run("clear_connections") == "0");
    REQUIRE(f.notices.size() == 1);
    CHECK(f.notices[0].destroyed.size() == 1);
    CHECK(f.connections() == nullptr);
    CHECK(f.run("set_connections_bad") == kInvalid);
    CHECK(f.connections() == nullptr);
}

TEST_CASE("edit op rebuild_solid_matched keeps side identity, material and the overlay's face") {
    EditFixture f;
    CHECK(f.run("set_material") == "0");
    REQUIRE(f.notices.size() == 1);
    CHECK((f.notices[0].modified[0].second & kMaterial) != 0);
    f.selectSolid();
    const auto sidesKey = f.key(f.entity, "sides");
    const auto before = f.document.undoCount();
    CHECK(f.run("rebuild_matched") == "0");
    CHECK(f.document.undoCount() == before + 1);
    CHECK(f.maxX() == doctest::Approx(256.0));
    for (const int id : f.sides) { CAPTURE(id); CHECK(f.document.side(id).has_value()); }
    REQUIRE(f.sideBlock(f.sides[4]));
    CHECK(nodeText(f.sideBlock(f.sides[4])->find("material")) == "DEV/TEST");  // material survived the rebuild
    CHECK(f.key(f.entity, "sides") == sidesKey);                                      // overlay still lists every face
    CHECK(f.notices.back().vanished.empty());
    CHECK(f.solidSelected());
    REQUIRE(f.document.undo());
    CHECK(f.maxX() == doctest::Approx(64.0));
    CHECK(f.key(f.entity, "sides") == sidesKey);
}

TEST_CASE("destroying a solid reports destroyed ids and vanished sides and prunes the overlay") {
    EditFixture f;
    f.selectSolid();
    const auto solidId = f.solidId();
    CHECK(f.run("destroy_solid") == "0");
    REQUIRE(f.notices.size() == 1);
    CHECK(f.notices[0].destroyed == std::vector<document::NodeId>{solidId});
    std::vector<int> vanished = f.notices[0].vanished, expected(f.sides.begin(), f.sides.end());
    std::ranges::sort(vanished); std::ranges::sort(expected);
    CHECK(vanished == expected);
    for (const int id : f.sides) CHECK_FALSE(f.document.side(id).has_value());
    const auto remaining = f.key(f.entity, "sides");
    for (const int id : f.sides) CHECK(remaining.find(std::to_string(id)) == std::string::npos);
    CHECK_FALSE(f.solidSelected());                         // the destroyed solid left the selection
    f.notices.clear(); f.plugin.clear();
    REQUIRE(f.document.undo());
    f.compare();
    REQUIRE(f.notices.size() == 1);
    CHECK(f.notices[0].undoOrRedo);
    CHECK(f.notices[0].created.size() >= 1);
    CHECK(f.notices[0].vanished.empty());
    for (const int id : f.sides) CHECK(f.document.side(id).has_value());
}

TEST_CASE("edit op create_block and set_editor") {
    EditFixture f;
    const auto undoBefore = f.document.undoCount();
    CHECK(f.run("create_block") == "0");
    CHECK(f.document.undoCount() == undoBefore + 1);
    REQUIRE(f.notices.size() == 1);
    REQUIRE(f.notices[0].created.size() == 1);
    const auto made = f.notices[0].created[0];
    CHECK(std::ranges::find(f.logs, "created_id=" + std::to_string(made)) != f.logs.end());
    const auto handle = f.document.handle(made);
    REQUIRE(handle);
    CHECK(f.key(*handle, "targetname") == "made");
    CHECK(f.key(*handle, "classname") == "info_target");
    CHECK(nodeText(f.document.get(*handle)->block->find("id")) != "0");  // the 0 placeholder was replaced
    f.notices.clear(); f.plugin.clear();
    CHECK(f.run("create_group") == "0");
    REQUIRE(f.notices.size() == 1);
    REQUIRE(f.notices[0].created.size() >= 1);        // the group and its editor block
    CHECK(std::ranges::any_of(f.notices[0].created, [&](document::NodeId id) {
        return f.document.get(*f.document.handle(id))->block->key() == "group";
    }));

    const auto editorBefore = f.document.undoCount();
    CHECK(f.run("editor_color") == "0");
    CHECK(f.document.undoCount() == editorBefore + 1);
    const auto* editorBlock = f.childBlock("editor");
    REQUIRE(editorBlock);
    CHECK(nodeText(editorBlock->find("color")) == "10 20 30");
    CHECK(f.run("editor_visgroup") == "0");
    REQUIRE(f.notices.size() == 1);
    REQUIRE(f.notices[0].modified.size() == 1);
    CHECK((f.notices[0].modified[0].second & kVisibility) != 0);
    CHECK(nodeText(f.childBlock("editor")->find("visgroupid")) == "7");
}

TEST_CASE("change notice reports every modified flag") {
    EditFixture f;
    const auto flagsOf = [&](const std::string& action, document::NodeId id) {
        CHECK(f.run(action) == "0");
        REQUIRE(f.notices.size() == 1);
        unsigned flags = 0;
        for (const auto& m : f.notices[0].modified) if (m.first == id) flags = m.second;
        return flags;
    };
    CHECK(flagsOf("set_key", f.entityId()) == kKeyValues);
    CHECK(f.key(f.entity, "targetname") == "renamed");
    CHECK((flagsOf("set_material", f.solidId()) & kMaterial) != 0);
    CHECK((flagsOf("set_geometry", f.solidId()) & kGeometry) != 0);
    CHECK((flagsOf("editor_visgroup", f.entityId()) & kVisibility) != 0);
    // Reparenting is a hierarchy change of the moved node.
    CHECK((flagsOf("reparent_entity", f.entityId()) & kHierarchy) != 0);
    CHECK(f.notices[0].created.empty());
    CHECK(f.notices[0].destroyed.empty());
}

TEST_CASE("one batch of several edit ops is one undo step and one notice") {
    EditFixture f;
    f.selectSolid();
    const auto before = f.document.undoCount();
    CHECK(f.run("everything") == "0");
    CHECK(f.document.undoCount() == before + 1);
    CHECK(f.notices.size() == 1);
    CHECK(f.key(f.entity, "targetname") == "renamed");
    CHECK(f.connections() != nullptr);
    CHECK(f.maxX() == doctest::Approx(128.0));
    CHECK(f.solidSelected());
    f.session.selection().clear();
    REQUIRE(f.document.undo());
    REQUIRE(editor::restoreFromUndo(f.document, f.session.selection()));
    CHECK(f.document.undoCount() == before);
    CHECK(f.key(f.entity, "targetname") == "old");
    CHECK(f.connections() == nullptr);
    CHECK(f.maxX() == doctest::Approx(64.0));
    CHECK(f.solidSelected());
    // Revisions keep increasing across edits and undo.
    f.notices.clear();
    CHECK(f.run("set_key") == "0");
    const auto first = f.notices.back().revision;
    CHECK(f.run("set_connections") == "0");
    CHECK(f.notices.back().revision > first);
}

// ---- I-6: tool state is per editor session -----------------------------------------------------------------
namespace {
editor::ToolRegistry::Factory toolFactory(std::string_view id) {
    for (const auto& entry : editor::toolRegistry().entries()) if (entry.id == id) return entry.factory;
    return {};
}
}
TEST_CASE("I-6 a tool with create_instance keeps independent state per editor session") {
    Temp temp;
    temp.manifest("toolstate", entryFor(TOOL_STATE_PLUGIN_PATH));
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
    host.discover({temp.path}); host.activate();
    REQUIRE(host.plugins()[0].loaded);
    REQUIRE(toolFactory("plugin.toolstate.own"));
    // Each session builds its own tool objects from the registry, so each gets its own instance.
    auto first = std::make_unique<editor::EditorSession>();
    auto second = std::make_unique<editor::EditorSession>();
    first->setActiveTool("plugin.toolstate.own"); second->setActiveTool("plugin.toolstate.own");
    auto& a = first->activeToolObject();
    auto& b = second->activeToolObject();
    a.perform("x"); a.perform("x"); b.perform("x");
    CHECK(a.readout().status == "own=2 live=2 created=2");
    CHECK(b.readout().status == "own=1 live=2 created=2");
    second.reset();                                 // destroy_instance runs with the session's tool
    CHECK(a.readout().status == "own=2 live=1 created=2");
    editor::EditorSession third;                    // a new session starts from a fresh instance
    third.setActiveTool("plugin.toolstate.own");
    CHECK(third.activeToolObject().readout().status == "own=0 live=2 created=3");
    for (const auto& line : logs) CHECK_FALSE((line.find("plugin.toolstate.own") != std::string::npos && line.find("shared between") != std::string::npos));
}

TEST_CASE("I-6 a legacy tool without create_instance shares its state and warns once") {
    Temp temp;
    temp.manifest("toolstate", entryFor(TOOL_STATE_PLUGIN_PATH));
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
    host.discover({temp.path}); host.activate();
    REQUIRE(host.plugins()[0].loaded);
    const auto warnings = [&] {
        return std::ranges::count_if(logs, [](const std::string& line) {
            return line.find("plugin.toolstate.shared") != std::string::npos && line.find("shared between editor sessions") != std::string::npos;
        });
    };
    editor::EditorSession first;
    CHECK(warnings() == 0);                         // one session is the common case and needs no warning
    editor::EditorSession second;
    CHECK(warnings() == 1);
    first.setActiveTool("plugin.toolstate.shared"); second.setActiveTool("plugin.toolstate.shared");
    first.activeToolObject().perform("x");
    CHECK(second.activeToolObject().readout().status == "count=1");   // documented behaviour: one shared instance
    editor::EditorSession third;
    CHECK(warnings() == 1);                         // warned once, not per session
    // The half-given callbacks were refused at registration, so that tool does not exist.
    CHECK(std::ranges::find(logs, "half refused") != logs.end());
    CHECK_FALSE(toolFactory("plugin.toolstate.half"));
}

TEST_CASE("wrapper edit-op helpers set struct_size and detect a short ops table") {
    const std::vector<apex_vec3> vertices{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const auto face = plugin::faceGeometry(7, {0, 1, 2}, apex_vec3{1, 0, 0}, apex_vec3{0, -1, 0});
    CHECK(face.struct_size == sizeof(apex_face_geometry));
    CHECK(face.side == 7);
    CHECK(face.index_count == 3);
    const std::vector<apex_face_geometry> faces{face};
    const auto solid = plugin::solidGeometry(vertices, faces);
    CHECK(solid.struct_size == sizeof(apex_solid_geometry));
    CHECK(solid.face_count == 1);
    CHECK(solid.vertex_count == 3);
    const auto link = plugin::connection("OnTrigger", "door", "Open");
    CHECK(link.struct_size == sizeof(apex_connection));
    CHECK(std::string_view(link.target.data, link.target.size) == "door");
    const auto match = plugin::faceMatch(2, 9);
    CHECK(match.struct_size == sizeof(apex_face_match));
    CHECK(match.plane_index == 2);
    CHECK(match.old_side == 9);

    apex_edit_ops_v1 ops{};
    ops.struct_size = sizeof(ops);
    CHECK(plugin::available(&ops, &apex_edit_ops_v1::set_editor) == nullptr);   // null pointer: not provided
    ops.set_editor = [](void*, apex_node_id, uint32_t, apex_str) { return APEX_OK; };
    ops.set_solid_geometry = [](void*, apex_node_id, const apex_solid_geometry*) { return APEX_OK; };
    CHECK(plugin::available(&ops, &apex_edit_ops_v1::set_editor) != nullptr);
    ops.struct_size = offsetof(apex_edit_ops_v1, set_solid_geometry);          // a host from before 0.9.3
    CHECK(plugin::available(&ops, &apex_edit_ops_v1::set_solid_geometry) == nullptr);
    CHECK(plugin::available(&ops, &apex_edit_ops_v1::set_editor) == nullptr);
    CHECK(plugin::available<decltype(ops.set_editor)>(nullptr, &apex_edit_ops_v1::set_editor) == nullptr);
}
