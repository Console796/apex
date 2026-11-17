#include "plugin_host/plugin_host.hpp"
#include "document/vmf.hpp"
#include "document/document.hpp"
#include "editor/editor_session.hpp"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <chrono>

using namespace apex;
using namespace apex::plugin_host;
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
