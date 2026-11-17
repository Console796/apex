// 0.9.7: script plugins. These tests drive the language-agnostic host side (manifest `script`/`runtime`,
// the ScriptEngine seam, reload on save) with FAKE engines. No AngelScript is involved: it was not
// obtainable offline, so nothing here proves the AngelScript binding, only the host plumbing it will use.
#include "plugin_host/plugin_host.hpp"
#include "plugin_host/plugin_tool.hpp"
#include "document/document.hpp"
#include "document/vmf.hpp"
#include "editor/editor_session.hpp"
#include "../editor/fixtures.hpp"
#include <apex/plugin.hpp>
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace apex;
using namespace apex::plugin_host;
namespace {
using clock_type = std::chrono::steady_clock;
constexpr auto ms = [](int n) { return std::chrono::milliseconds(n); };

// Process-wide record of what the fake engines did. Reset at the start of each test case.
struct Probe {
    int loads = 0, unloads = 0, enginesAlive = 0, instancesMade = 0, instancesDestroyed = 0;
    std::vector<std::string> events;
};
Probe probe;

struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("apex-script-test-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directories(path); }
    ~Temp() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    // `fields` is the manifest text after the id/name/abi, for example: script = "main.fake" runtime = "fake"
    void manifest(std::string_view id, std::string_view fields) {
        const auto dir = path / id;
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "plugin.kv3") << "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n"
            << "{ id = \"" << id << "\" name = \"Script\" version = \"1.0.0\" apex_abi = { major = 1 minor = 0 } " << fields << " }";
    }
    void script(std::string_view id, std::string_view runtime, std::string_view body, std::string_view file = "main.fake") {
        manifest(id, "script = \"" + std::string(file) + "\" runtime = \"" + std::string(runtime) + "\"");
        write(id, body, file);
    }
    // Writes the script and moves its mtime forward so every save is a distinct change.
    void write(std::string_view id, std::string_view body, std::string_view file = "main.fake") {
        const auto target = path / id / file;
        std::ofstream(target, std::ios::trunc) << body;
        static int bump = 0;
        std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(++bump * 5));
    }
};

// A tool whose behaviour is the label it was registered with; instances are counted.
class TextTool final : public plugin::Tool {
public:
    TextTool(std::string label, bool instance) : label_(std::move(label)), instance_(instance) { if (instance_) ++probe.instancesMade; }
    ~TextTool() override { if (instance_) ++probe.instancesDestroyed; }
    Tool* createInstance() override { return new TextTool(label_, true); }
    bool input(const apex_tool_event&, const apex_tool_view&) override { probe.events.push_back("input:" + label_); return true; }
private:
    std::string label_;
    bool instance_;
};
// A stair: `steps` boxes, each a convex ghost of six planes, redrawn on every refresh (the P5 shape).
class StairTool final : public plugin::Tool {
public:
    explicit StairTool(int steps) : steps_(steps) {}
    Tool* createInstance() override { return new StairTool(steps_); }
    bool input(const apex_tool_event&, const apex_tool_view&) override { return true; }
    apex_status visual(uint64_t, apex_tool_visual& out) override {
        segments_.clear(); planes_.clear(); ghosts_.clear();
        planes_.reserve(static_cast<std::size_t>(steps_) * 6);
        for (int i = 0; i < steps_; ++i) {
            const double x0 = i * 8.0, x1 = x0 + 8.0, z = (i + 1) * 8.0;
            const std::size_t first = planes_.size();
            planes_.push_back({{1, 0, 0}, x1}); planes_.push_back({{-1, 0, 0}, -x0});
            planes_.push_back({{0, 1, 0}, 64}); planes_.push_back({{0, -1, 0}, 0});
            planes_.push_back({{0, 0, 1}, z});  planes_.push_back({{0, 0, -1}, 0});
            segments_.push_back({{x0, 0, z}, {x1, 0, z}, {1, 1, 1, 1}, 2});
            ghosts_.push_back({planes_.data() + first, 6, plugin::string("DEV/DEV_MEASUREGENERIC01"), 0.35f});
        }
        out.segments = segments_.data(); out.segment_count = segments_.size();
        out.ghosts = ghosts_.data(); out.ghost_count = ghosts_.size();
        return APEX_OK;
    }
private:
    int steps_;
    std::vector<apex_tool_segment> segments_;
    std::vector<apex_plane> planes_;
    std::vector<apex_tool_ghost> ghosts_;
};

// Reads a tiny text script and registers what it says through the real host ABI, the way a language
// binding would. Directives, one per line: `tool NAME LABEL`, `action NAME LABEL`, `stair NAME STEPS`,
// `throw` (the load throws), `fail MESSAGE` (the load returns false). Anything else is a syntax error.
class FakeEngine final : public ScriptEngine {
public:
    FakeEngine() { ++probe.enginesAlive; }
    ~FakeEngine() override { --probe.enginesAlive; }
    bool load(const ScriptSource& source, const apex_host* host, std::string& error) override {
        std::ifstream in(source.file);
        if (!in) { error = "cannot read " + source.file.string(); return false; }
        const auto* tools = plugin::tools(*host);
        const auto* actions = static_cast<const apex_actions_v1*>(host->get_interface(host->context, plugin::string("apex.actions/1")));
        std::string line;
        for (int n = 1; std::getline(in, line); ++n) {
            std::istringstream words(line);
            std::string verb, name, rest;
            words >> verb >> name;
            std::getline(words, rest);
            if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            if (verb.empty() || verb[0] == '#') continue;
            const auto id = "plugin." + source.pluginId + "." + name;
            if (verb == "throw") throw std::runtime_error("boom at line " + std::to_string(n));
            if (verb == "fail") { error = "line " + std::to_string(n) + ": " + name + (rest.empty() ? "" : " " + rest); return false; }
            if (verb == "tool" || verb == "stair") {
                auto tool = verb == "tool" ? std::unique_ptr<plugin::Tool>(new TextTool(rest, false))
                                           : std::unique_ptr<plugin::Tool>(new StairTool(std::stoi(rest)));
                if (tools->register_tool(host->context, plugin::string(id), plugin::string(name), plugin::string("script tool"),
                                         tool.get(), plugin::Tool::callbacks()) != APEX_OK) { error = "line " + std::to_string(n) + ": register_tool refused " + id; return false; }
                tools_.push_back(std::move(tool));
            } else if (verb == "action") {
                labels_.push_back(std::make_unique<std::string>(rest));
                if (actions->register_action(host->context, plugin::string(id), plugin::string(name), plugin::string("script action"),
                                             labels_.back().get(), [](void* user) { probe.events.push_back("action:" + *static_cast<std::string*>(user)); },
                                             nullptr, 0) != APEX_OK) { error = "line " + std::to_string(n) + ": register_action refused " + id; return false; }
            } else {
                error = "line " + std::to_string(n) + ": unknown directive '" + verb + "'";
                return false;
            }
        }
        ++probe.loads;
        return true;
    }
    void unload() override { ++probe.unloads; tools_.clear(); labels_.clear(); }
private:
    std::vector<std::unique_ptr<plugin::Tool>> tools_;
    std::vector<std::unique_ptr<std::string>> labels_;
};

// Test-only engine that runs a NATIVE plugin library as if it were a script: it loads the library, hands
// it the host's apex_host and calls activate, so the polygon example registers itself through exactly the
// path a script would use. It proves script plumbing equals native plumbing; it is not a scripting language.
class BridgeEngine final : public ScriptEngine {
public:
    bool load(const ScriptSource&, const apex_host* host, std::string& error) override {
#ifdef _WIN32
        void* lib = reinterpret_cast<void*>(LoadLibraryA(POLYGON_PLUGIN_PATH));
        auto* entry = lib ? reinterpret_cast<apex_plugin_entry_fn>(GetProcAddress(reinterpret_cast<HMODULE>(lib), "apex_plugin_entry")) : nullptr;
#else
        void* lib = dlopen(POLYGON_PLUGIN_PATH, RTLD_NOW | RTLD_LOCAL);
        auto* entry = lib ? reinterpret_cast<apex_plugin_entry_fn>(dlsym(lib, "apex_plugin_entry")) : nullptr;
#endif
        if (!entry) { error = "bridge cannot load " POLYGON_PLUGIN_PATH; return false; }
        plugin_ = entry(host);   // the library stays mapped for the process lifetime on purpose
        if (!plugin_ || !plugin_->activate || plugin_->activate(plugin_->user) != APEX_OK) { error = "bridge activate failed"; return false; }
        return true;
    }
    void unload() override { if (plugin_ && plugin_->deactivate) plugin_->deactivate(plugin_->user); plugin_ = nullptr; }
private:
    const apex_plugin* plugin_ = nullptr;
};

ScriptEngineFactory factory() {
    return [](std::string_view runtime) -> std::unique_ptr<ScriptEngine> {
        if (runtime == "fake") return std::make_unique<FakeEngine>();
        if (runtime == "bridge") return std::make_unique<BridgeEngine>();
        return nullptr;
    };
}
const Plugin* find(const Host& host, std::string_view id) {
    for (const auto& p : host.plugins()) if (p.id == id) return &p;
    return nullptr;
}
std::size_t toolEntries(std::string_view id) {
    return static_cast<std::size_t>(std::ranges::count(editor::toolRegistry().entries(), id, &editor::ToolRegistry::Entry::id));
}
// Saves have settled once the debounce has passed with no further change; returns reloads delivered.
std::size_t settle(Host& host, clock_type::time_point& now) {
    host.pollWatches(now += ms(10));
    return host.pollWatches(now += ms(300));
}
editor::InputEvent press() {
    editor::InputEvent event; event.source = 7; event.kind = editor::InputKind::Press; event.button = editor::PointerButton::Left;
    return event;
}
}

TEST_CASE("script manifests are validated at discovery") {
    probe = {};
    Temp temp;
    temp.script("ok", "fake", "tool t v1\n");
    temp.manifest("absolute", "script = \"/etc/passwd\" runtime = \"fake\"");
    temp.manifest("dotdot", "script = \"../outside.fake\" runtime = \"fake\"");
    temp.manifest("both", "script = \"main.fake\" runtime = \"fake\" entry = { \"linux-x86_64\" = \"x\" }");
    temp.manifest("noruntime", "script = \"main.fake\"");
    temp.manifest("nofile", "script = \"absent.fake\" runtime = \"fake\"");
    temp.manifest("emptyname", "script = \"\" runtime = \"fake\"");
    std::filesystem::create_directories(temp.path / "dotdot");
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.discover({temp.path});
    REQUIRE(host.plugins().size() == 7);
    const auto* ok = find(host, "ok");
    REQUIRE(ok);
    CHECK(ok->error.empty());
    CHECK(ok->runtime == "fake");
    CHECK(ok->script == temp.path / "ok" / "main.fake");
    CHECK(ok->entry.empty());
    const auto error = [&](std::string_view id) { const auto* p = find(host, id); REQUIRE(p); return p->error; };
    CHECK(error("absolute").find("inside the plugin folder") != std::string::npos);
    CHECK(error("dotdot").find("inside the plugin folder") != std::string::npos);
    CHECK(error("both").find("either entry or script") != std::string::npos);
    CHECK(error("noruntime").find("runtime") != std::string::npos);
    CHECK(error("nofile").find("absent.fake") != std::string::npos);
    CHECK(error("emptyname").find("script") != std::string::npos);
}

TEST_CASE("a script plugin without an engine stays listed with an error and other plugins load") {
    probe = {};
    Temp temp;
    temp.script("alpha", "fake", "tool t v1\n");
    temp.script("zebra", "nonesuch", "tool t v1\n");
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
    host.setScriptEngineFactory(factory());
    host.discover({temp.path});
    host.activate();
    const auto* alpha = find(host, "alpha");
    const auto* zebra = find(host, "zebra");
    REQUIRE(alpha); REQUIRE(zebra);
    CHECK(alpha->loaded);
    CHECK_FALSE(zebra->loaded);
    CHECK(zebra->error.find("nonesuch") != std::string::npos);
    CHECK(std::ranges::any_of(logs, [](const std::string& l) { return l.find("zebra") != std::string::npos && l.find("nonesuch") != std::string::npos; }));
    CHECK(toolEntries("plugin.alpha.t") == 1);
    CHECK(toolEntries("plugin.zebra.t") == 0);

    // No factory at all behaves the same: listed, errored, no crash.
    Host bare(temp.path / "state2", temp.path / "breadcrumb2");
    bare.discover({temp.path});
    bare.activate();
    REQUIRE(find(bare, "alpha"));
    CHECK_FALSE(find(bare, "alpha")->loaded);
    CHECK(find(bare, "alpha")->error.find("fake") != std::string::npos);
}

namespace {
// The 0.9.2 polygon scenarios, run against a `script` manifest whose engine is the bridge.
struct ScriptPolygon {
    Temp temp;
    document::Document document{editor_test::parsed("world { id 1 classname worldspawn }")};
    Host host{temp.path / "state", temp.path / "breadcrumb"};
    ScriptPolygon() {
        temp.script("polygon", "bridge", "# the bridge engine ignores the file\n");
        host.setScriptEngineFactory(factory());
        host.discover({temp.path});
        host.setDocument(&document);
        host.activate();
    }
    ~ScriptPolygon() { host.setDocument(nullptr); host.setSession(nullptr); }
};
}

TEST_CASE("a script polygon tool records the same trace and lifecycle as the native one") {
    ScriptPolygon fixture;
    auto& host = fixture.host;
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    REQUIRE(host.plugins()[0].loaded);
    editor::EditorSession session;
    session.attach(fixture.document);
    REQUIRE(session.perform("plugin.polygon.tool"));
    editor::ToolView view;
    view.cursorRay = {{0, 0, 64}, {0, 0, -1}};
    auto event = press();
    event.source = 42;
    for (const auto& xy : {DVec2{0, 0}, DVec2{64, 0}, DVec2{0, 64}}) {
        view.cursorRay.origin = {xy.x, xy.y, 64};
        CHECK(session.input(event, view));
    }
    const auto first = session.activeToolObject().overlay(view, 42);
    CHECK(first.worldSegments.size() == 3);
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
    event.kind = editor::InputKind::Press;
    event.key.clear();
    for (const auto& xy : {DVec2{0, 0}, DVec2{64, 0}, DVec2{0, 64}}) {
        view.cursorRay.origin = {xy.x, xy.y, 64};
        CHECK(session.input(event, view));
    }
    const auto& replay = session.activeToolObject().preview().ghosts();
    REQUIRE(replay.size() == firstGhosts.size());
    CHECK(replay[0].poly.vertices == firstGhosts[0].poly.vertices);
    session.forgetInputSource(42);
    CHECK(session.activeToolObject().overlay(view, 42).worldSegments.empty());
}

TEST_CASE("a script polygon commit is one undoable edit preserving selection") {
    ScriptPolygon fixture;
    auto& host = fixture.host;
    auto& document = fixture.document;
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
    const auto event = press();
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
}

TEST_CASE("script tool state is independent between editor sessions") {
    ScriptPolygon fixture;
    REQUIRE(fixture.host.plugins()[0].loaded);
    editor::EditorSession first, second;
    first.attach(fixture.document); second.attach(fixture.document);
    REQUIRE(first.perform("plugin.polygon.tool"));
    REQUIRE(second.perform("plugin.polygon.tool"));
    editor::ToolView view;
    view.cursorRay = {{0, 0, 64}, {0, 0, -1}};
    auto event = press();
    event.source = 42;
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
}

TEST_CASE("editing a script reloads it and its registrations without leaking the old ones") {
    probe = {};
    Temp temp;
    temp.script("reload1", "fake", "tool t v1\naction a v1\ntool gone v1\n");
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
    host.setScriptEngineFactory(factory());
    host.discover({temp.path});
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    CHECK(probe.loads == 1);
    CHECK(probe.enginesAlive == 1);
    CHECK(host.hasWatches());

    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    host.setDocument(&document); host.setSession(&session);
    editor::ToolView view;
    const auto event = press();
    REQUIRE(session.perform("plugin.reload1.t"));
    CHECK(session.input(event, view));
    CHECK(probe.events.back() == "input:v1");
    REQUIRE(session.perform("plugin.reload1.a"));
    CHECK(probe.events.back() == "action:v1");
    CHECK(probe.instancesMade == 2);       // the session makes one instance per registered tool (t and gone)
    CHECK(probe.instancesDestroyed == 0);

    // Save a new version: one reload, after the debounce.
    temp.write("reload1", "tool t v2\naction a v2\ntool fresh v2\n");
    auto now = clock_type::now();
    CHECK(host.pollWatches(now) == 0);   // noticed, still inside the debounce window
    CHECK(host.pollWatches(now += ms(300)) == 1);
    CHECK(probe.loads == 2);
    CHECK(probe.unloads == 1);
    CHECK(probe.enginesAlive == 1);                 // the old engine was freed, only the new one remains
    CHECK(probe.instancesDestroyed == 2);           // both old tool instances were retired and destroyed
    CHECK(probe.instancesMade == 3);                // and the surviving id got a fresh one (revived)
    CHECK(std::ranges::any_of(logs, [](const std::string& l) { return l.find("reloaded") != std::string::npos; }));
    CHECK(host.plugins()[0].loaded);
    CHECK(host.plugins()[0].error.empty());

    // The session keeps its tool object for `t`: it was rebound to v2 (same id, fresh instance) and, being
    // the active tool, activated again. It never runs v1 code.
    CHECK(session.activeTool() == "plugin.reload1.t");
    CHECK(session.input(event, view));
    CHECK(probe.events.back() == "input:v2");
    CHECK(toolEntries("plugin.reload1.t") == 1);
    session.setActiveTool(editor::EditorSession::Tool::Select);
    REQUIRE(session.perform("plugin.reload1.t"));
    CHECK(session.input(event, view));
    CHECK(probe.events.back() == "input:v2");
    REQUIRE(session.perform("plugin.reload1.a"));
    CHECK(probe.events.back() == "action:v2");
    CHECK(toolEntries("plugin.reload1.t") == 1);

    // An id the new version adds works while the registries are open; one it drops is inert, not dangling.
    CHECK(toolEntries("plugin.reload1.fresh") == 1);
    {
        editor::EditorSession later;   // sessions made after the reload see the new tool
        later.attach(document);
        REQUIRE(later.perform("plugin.reload1.fresh"));
        CHECK(later.input(event, view));
        CHECK(probe.events.back() == "input:v2");
    }
    session.setActiveTool(editor::EditorSession::Tool::Select);
    REQUIRE(session.perform("plugin.reload1.gone"));
    const auto beforeGone = probe.events.size();
    CHECK_FALSE(session.input(event, view));
    CHECK(probe.events.size() == beforeGone);
    CHECK(toolEntries("plugin.reload1.gone") == 1);

    // Quiet afterwards: no repeat. A third save is exactly one more reload (no duplicate watches).
    CHECK(settle(host, now) == 0);
    temp.write("reload1", "tool t v3\naction a v3\n");
    CHECK(settle(host, now) == 1);
    CHECK(probe.loads == 3);
    CHECK(probe.unloads == 2);
    CHECK(probe.enginesAlive == 1);
    CHECK(settle(host, now) == 0);
    CHECK(probe.loads == 3);
    CHECK(host.hasWatches());
    session.setActiveTool(editor::EditorSession::Tool::Select);
    REQUIRE(session.perform("plugin.reload1.t"));
    CHECK(session.input(event, view));
    CHECK(probe.events.back() == "input:v3");
    host.setSession(nullptr); host.setDocument(nullptr);
}

TEST_CASE("a broken script reports an error, never crashes, and a fixed save recovers") {
    probe = {};
    Temp temp;
    temp.script("err1", "fake", "tool t v1\n");
    temp.script("steady", "fake", "tool t ok\n");
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
    host.setScriptEngineFactory(factory());
    host.discover({temp.path});
    host.activate();
    REQUIRE(find(host, "err1")->loaded);
    REQUIRE(find(host, "steady")->loaded);
    editor::EditorSession session;
    editor::ToolView view;
    const auto event = press();
    auto now = clock_type::now();
    const auto reload = [&](std::string_view body) {
        temp.write("err1", body);
        return settle(host, now);
    };

    // Syntax error: the plugin goes unloaded with a located message; the other plugin is untouched.
    CHECK(reload("tool t v2\nbogus line\n") == 1);
    CHECK_FALSE(find(host, "err1")->loaded);
    CHECK(find(host, "err1")->error.find("line 2") != std::string::npos);
    CHECK(std::ranges::any_of(logs, [](const std::string& l) { return l.find("err1") != std::string::npos && l.find("line 2") != std::string::npos; }));
    CHECK(probe.enginesAlive == 1);   // the failed engine was released, only `steady` remains
    REQUIRE(session.perform("plugin.err1.t"));
    CHECK_FALSE(session.input(event, view));   // inert, not the old v1 code
    CHECK(std::ranges::none_of(probe.events, [](const std::string& e) { return e == "input:v1" || e == "input:v2"; }));
    session.setActiveTool(editor::EditorSession::Tool::Select);
    REQUIRE(session.perform("plugin.steady.t"));
    CHECK(session.input(event, view));
    CHECK(probe.events.back() == "input:ok");

    // An exception thrown by the engine, and an explicit failure, are contained the same way.
    CHECK(reload("throw\n") == 1);
    CHECK_FALSE(find(host, "err1")->loaded);
    CHECK(find(host, "err1")->error.find("boom") != std::string::npos);
    CHECK(reload("fail custom message\n") == 1);
    CHECK(find(host, "err1")->error.find("custom message") != std::string::npos);

    // A fixed save recovers the plugin and rebinds the existing registry entry.
    CHECK(reload("tool t v3\n") == 1);
    CHECK(find(host, "err1")->loaded);
    CHECK(find(host, "err1")->error.empty());
    CHECK(toolEntries("plugin.err1.t") == 1);
    session.setActiveTool(editor::EditorSession::Tool::Select);
    REQUIRE(session.perform("plugin.err1.t"));
    CHECK(session.input(event, view));
    CHECK(probe.events.back() == "input:v3");
}

TEST_CASE("a script that fails its first load is listed with its error and reloads once fixed") {
    probe = {};
    Temp temp;
    temp.script("late", "fake", "throw\n");
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.setScriptEngineFactory(factory());
    host.discover({temp.path});
    REQUIRE_NOTHROW(host.activate());
    REQUIRE(find(host, "late"));
    CHECK_FALSE(find(host, "late")->loaded);
    CHECK(find(host, "late")->error.find("boom") != std::string::npos);
    CHECK(probe.enginesAlive == 0);
    temp.write("late", "tool t v1\n");
    auto now = clock_type::now();
    CHECK(settle(host, now) == 1);
    CHECK(find(host, "late")->loaded);
    CHECK(toolEntries("plugin.late.t") == 1);
    // reloadScript is the explicit form of the same thing, and refuses what is not a loaded script.
    std::string message;
    CHECK(host.reloadScript("late", &message));
    CHECK_FALSE(host.reloadScript("nonexistent", &message));
    CHECK(message.find("not a loaded script") != std::string::npos);
}

TEST_CASE("P5 host-side floor for a 64-step stair (opt-in, excludes script execution)") {
    // Set APEX_MEASURE_SCRIPT=1 to run. This measures only what the HOST costs per refresh when a tool
    // returns 64 ghosts: the native StairTool above stands in for the script. The 2 ms budget in the
    // plan is for the SCRIPT version and is NOT established by this test.
    if (!std::getenv("APEX_MEASURE_SCRIPT")) return;
    probe = {};
    Temp temp;
    temp.script("stairs", "fake", "stair s 64\n");
    Host host(temp.path / "state", temp.path / "breadcrumb");
    host.setScriptEngineFactory(factory());
    host.discover({temp.path});
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    REQUIRE(session.perform("plugin.stairs.s"));
    editor::ToolView view;
    const auto event = press();
    constexpr int runs = 200;
    for (int i = 0; i < 10; ++i) { session.input(event, view); session.activeToolObject().overlay(view, 7); }
    const auto start = clock_type::now();
    for (int i = 0; i < runs; ++i) { session.input(event, view); session.activeToolObject().overlay(view, 7); }
    const double perRefreshMs = std::chrono::duration<double, std::milli>(clock_type::now() - start).count() / runs;
    MESSAGE("host-side floor per input+refresh with 64 ghosts: " << perRefreshMs << " ms");
    CHECK(session.activeToolObject().preview().ghosts().size() == 64);
    CHECK(perRefreshMs < 2.0);
}
