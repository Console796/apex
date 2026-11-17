#include "plugin_host/plugin_host.hpp"
#include "plugin_host/plugin_tool.hpp"
#include "document/vmf.hpp"
#include "document/document.hpp"
#include "editor/editor_session.hpp"
#include "editor/map_problems.hpp"
#include "../editor/fixtures.hpp"
#include "geometry/solid_geometry.hpp"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <algorithm>

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
        UiContext context{"materials", {"dev/a", "dev/b"}, {}};
        std::string summary;
        const auto describeSection = [&](const UiContext& c) {
            return host.describePanel("plugin.ui.section", &c, [&](const apex_ui_form& form) {
                summary = std::string(form.controls[0].value.text.data, form.controls[0].value.text.size);
            });
        };
        REQUIRE(describeSection(context));
        CHECK(summary == "materials names=2 ids=0 first=dev/a");
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
