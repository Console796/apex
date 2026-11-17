// 0.9.7: script plugins. Two layers:
//  * the language-agnostic host side (manifest `script`/`runtime`, the ScriptEngine seam, reload on save),
//    driven with FAKE engines and a bridge that runs the native polygon example as if it were a script;
//  * the real AngelScript engine (plugin_host/angelscript_engine.hpp), exercised through the same session
//    API as a native tool: a script polygon, a broken-script matrix, leak-free reload and the stair example.
// The real-engine cases exist only when APEX_WITH_ANGELSCRIPT is ON (APEX_HAVE_ANGELSCRIPT); with it OFF
// they are compiled out and the polygon cases fall back to the bridge.
#include "plugin_host/plugin_host.hpp"
#include "plugin_host/plugin_tool.hpp"
#include "plugin_host/angelscript_engine.hpp"
#include "document/document.hpp"
#include "document/vmf.hpp"
#include "editor/editor_session.hpp"
#include "../editor/fixtures.hpp"
#include <apex/plugin.hpp>
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
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
            segments_.push_back(plugin::segment({x0, 0, z}, {x1, 0, z}, 2, {1, 1, 1, 1}));
            ghosts_.push_back(plugin::ghost(planes_.data() + first, 6, 0.35f, "DEV/DEV_MEASUREGENERIC01"));
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
        if (runtime == "angelscript" && angelScriptCompiledIn()) return defaultScriptEngineFactory()(runtime);
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
// The native polygon example (examples/polygon-tool/polygon.cpp) written as an AngelScript, same ids and
// behaviour: per input-source points, closed outline, a prism ghost of n side planes + top/bottom,
// Enter/commit through one apex.edit batch.
constexpr const char* kPolygonScript = R"AS(
class Polygon {
    array<uint64> sources;
    array<array<apex::Vec3>@> lists;
    uint64 last = 0;
    int slot(uint64 s) { for (uint i = 0; i < sources.length(); i++) if (sources[i] == s) return int(i); return -1; }
    bool input(apex::Event@ ev, apex::View@ view) {
        if (ev.kind == apex::InputKind::KeyDown && ev.key == "Escape") { cancel(); return true; }
        if (ev.kind == apex::InputKind::KeyDown && ev.key == "Enter") return perform("plugin.polygon.commit");
        if (ev.kind != apex::InputKind::Press || ev.button != apex::Button::Left || abs(view.rayDirection.z) < 1e-9) return false;
        last = ev.source;
        double t = -view.rayOrigin.z / view.rayDirection.z;
        if (t < 0) return false;
        int i = slot(ev.source);
        if (i < 0) { sources.insertLast(ev.source); lists.insertLast(array<apex::Vec3>()); i = int(sources.length()) - 1; }
        lists[i].insertLast(apex::Vec3(view.rayOrigin.x + t * view.rayDirection.x, view.rayOrigin.y + t * view.rayDirection.y, 0));
        return true;
    }
    array<apex::Plane>@ prism(uint64 s) {
        int i = slot(s);
        if (i < 0 || lists[i].length() < 3) return null;
        array<apex::Vec3>@ p = lists[i];
        uint n = p.length();
        double area = 0;
        for (uint k = 0; k < n; k++) { apex::Vec3 a = p[k], b = p[(k + 1) % n]; area += a.x * b.y - b.x * a.y; }
        if (abs(area) <= 1e-9) return null;
        double sign = area > 0 ? 1 : -1;
        array<apex::Plane> planes;
        for (uint k = 0; k < n; k++) {
            apex::Vec3 a = p[k], b = p[(k + 1) % n];
            double len = sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
            if (len < 1e-9) return null;
            apex::Vec3 nn(sign * (b.y - a.y) / len, sign * (a.x - b.x) / len, 0);
            planes.insertLast(apex::Plane(nn, nn.x * a.x + nn.y * a.y));
        }
        planes.insertLast(apex::Plane(apex::Vec3(0, 0, 1), 16));
        planes.insertLast(apex::Plane(apex::Vec3(0, 0, -1), 0));
        return planes;
    }
    void visual(uint64 s, apex::Visual@ v) {
        int i = slot(s);
        if (i < 0) return;
        array<apex::Vec3>@ p = lists[i];
        if (p.length() >= 2) for (uint k = 0; k < p.length(); k++) v.line(p[k], p[(k + 1) % p.length()], 1, 0.65, 0.15, 1, 2);
        array<apex::Plane>@ planes = prism(s);
        if (planes !is null) v.ghost(planes, "DEV/DEV_MEASUREGENERIC01", 0.35);
    }
    void readout(apex::Readout@ r) { r.detail("script polygon"); }
    bool perform(const string &in action) {
        if (action != "plugin.polygon.commit") return false;
        array<apex::Plane>@ planes = prism(last);
        if (planes is null || !apex::documentOpen()) return false;
        apex::Edit@ e = apex::Edit();
        e.createSolid(apex::root(0), planes, "DEV/DEV_MEASUREGENERIC01");
        if (!e.commit("Create polygon")) return false;
        cancel();
        return true;
    }
    void cancel() { sources.resize(0); lists.resize(0); }
    void forget(uint64 s) { int i = slot(s); if (i >= 0) { sources.removeAt(i); lists.removeAt(i); } }
}
void main() { apex::registerTool("tool", "Polygon tool", "Preview a polygon prism", "Polygon"); }
)AS";

// The 0.9.2 polygon scenarios, run against a `script` manifest. With the real engine compiled in the
// plugin is the AngelScript polygon above; otherwise it is the bridge that runs the native library.
struct ScriptPolygon {
    Temp temp;
    document::Document document{editor_test::parsed("world { id 1 classname worldspawn }")};
    Host host{temp.path / "state", temp.path / "breadcrumb"};
    // `id` is the plugin id of the script version; the native library always registers plugin.polygon.*.
    std::string id;
    explicit ScriptPolygon(bool real = angelScriptCompiledIn(), std::string pluginId = "polygon") : id(std::move(pluginId)) {
        if (real) {
            std::string text = kPolygonScript;
            const std::string replacement = "plugin." + id + ".";
            for (auto at = text.find("plugin.polygon."); at != std::string::npos; at = text.find("plugin.polygon.", at + replacement.size()))
                text.replace(at, 15, replacement);
            temp.script(id, "angelscript", text, "polygon.as");
        } else temp.script("polygon", "bridge", "# the bridge engine ignores the file\n");
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

#if defined(APEX_HAVE_ANGELSCRIPT)
namespace {
std::string logText(const std::vector<std::string>& logs) { std::string all; for (const auto& l : logs) all += l + "\n"; return all; }
bool logged(const std::vector<std::string>& logs, std::string_view needle) {
    return std::ranges::any_of(logs, [&](const std::string& l) { return l.find(needle) != std::string::npos; });
}
// The script computes a side length with sqrt and the native example with hypot, which can differ in the
// last bit; everything else must be identical. Compares two texts token by token, numbers within 1e-6.
bool sameWithin(const std::string& a, const std::string& b, double eps = 1e-6) {
    const auto tokens = [](const std::string& text) {
        std::vector<std::string> out;
        std::string current;
        for (char c : text) {
            if (std::string_view(" \t\r\n()[]\"").find(c) != std::string_view::npos) { if (!current.empty()) out.push_back(std::move(current)); current.clear(); }
            else current += c;
        }
        if (!current.empty()) out.push_back(std::move(current));
        return out;
    };
    const auto ta = tokens(a), tb = tokens(b);
    if (ta.size() != tb.size()) return false;
    for (std::size_t i = 0; i < ta.size(); ++i) {
        char* endA = nullptr; char* endB = nullptr;
        const double x = std::strtod(ta[i].c_str(), &endA), y = std::strtod(tb[i].c_str(), &endB);
        const bool numA = *endA == 0 && endA != ta[i].c_str(), numB = *endB == 0 && endB != tb[i].c_str();
        if (numA != numB) return false;
        if (numA ? std::abs(x - y) > eps : ta[i] != tb[i]) return false;
    }
    return true;
}
// Runs the fixed three-click polygon through a session and returns what the viewport would show.
struct PolygonRun {
    std::vector<std::pair<DVec3, DVec3>> segments;
    std::vector<std::vector<DVec3>> ghostVertices;
    std::string vmf;
    std::size_t undoDelta = 0;
};
PolygonRun runPolygon(ScriptPolygon& fixture, const std::string& pluginId) {
    PolygonRun run;
    editor::EditorSession session;
    session.attach(fixture.document);
    fixture.host.setSession(&session);
    REQUIRE(session.perform("plugin." + pluginId + ".tool"));
    editor::ToolView view;
    view.cursorRay = {{0, 0, 64}, {0, 0, -1}};
    const auto event = press();
    for (const auto& xy : {DVec2{0, 0}, DVec2{64, 0}, DVec2{48, 80}, DVec2{0, 64}}) {
        view.cursorRay.origin = {xy.x, xy.y, 64};
        REQUIRE(session.input(event, view));
    }
    for (const auto& s : session.activeToolObject().overlay(view, 7).worldSegments) run.segments.emplace_back(s.a, s.b);
    for (const auto& g : session.activeToolObject().preview().ghosts()) run.ghostVertices.push_back(g.poly.vertices);
    const auto before = fixture.document.undoCount();
    REQUIRE(session.perform("plugin." + pluginId + ".commit"));
    run.undoDelta = fixture.document.undoCount() - before;
    auto file = fixture.document.toVmf();
    REQUIRE(file);
    run.vmf = vmf::write(*file);
    fixture.host.setSession(nullptr);
    return run;
}
}

TEST_CASE("a real AngelScript polygon is indistinguishable from the native polygon") {
    const auto liveBefore = liveAngelScriptEngines();
    // The tool registry is process-wide and the native library fixes its id (plugin.polygon.*), so the
    // script gets its own id and the two run one after the other.
    PolygonRun a, b;
    {
        ScriptPolygon native(false);
        REQUIRE(native.host.plugins()[0].loaded);
        a = runPolygon(native, "polygon");
    }
    {
        ScriptPolygon script(true, "polyscript");
        CAPTURE(script.host.plugins()[0].error);
        REQUIRE(script.host.plugins()[0].loaded);
        CHECK(liveAngelScriptEngines() == liveBefore + 1);
        b = runPolygon(script, "polyscript");
    }
    CHECK(liveAngelScriptEngines() == liveBefore);
    REQUIRE(a.segments.size() == 4);
    CHECK(b.segments == a.segments);
    REQUIRE(a.ghostVertices.size() == 1);
    REQUIRE(b.ghostVertices.size() == a.ghostVertices.size());
    for (std::size_t g = 0; g < a.ghostVertices.size(); ++g) {
        REQUIRE(b.ghostVertices[g].size() == a.ghostVertices[g].size());
        for (std::size_t v = 0; v < a.ghostVertices[g].size(); ++v) {
            CHECK(std::abs(b.ghostVertices[g][v].x - a.ghostVertices[g][v].x) < 1e-6);
            CHECK(std::abs(b.ghostVertices[g][v].y - a.ghostVertices[g][v].y) < 1e-6);
            CHECK(std::abs(b.ghostVertices[g][v].z - a.ghostVertices[g][v].z) < 1e-6);
        }
    }
    CHECK(a.undoDelta == 1);
    CHECK(b.undoDelta == 1);
    CHECK(sameWithin(a.vmf, b.vmf));  // identical committed planes, ids and materials (to 1e-6)
    CHECK(b.vmf.find("solid") != std::string::npos);
}

TEST_CASE("script registrations are namespaced and the engine is freed with the host") {
    const auto liveBefore = liveAngelScriptEngines();
    {
        Temp temp;
        temp.script("ns", "angelscript",
            "class T { bool input(apex::Event@, apex::View@) { return true; } }\n"
            "void a() { apex::log(\"acted\"); }\n"
            "void main() { apex::registerTool(\"t\", \"T\", \"d\", \"T\"); apex::registerAction(\"a\", \"A\", \"d\", a); }\n",
            "ns.as");
        std::vector<std::string> logs;
        Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
        host.setScriptEngineFactory(factory());
        host.discover({temp.path});
        host.activate();
        CAPTURE(host.plugins()[0].error);
        REQUIRE(host.plugins()[0].loaded);
        CHECK(liveAngelScriptEngines() == liveBefore + 1);
        CHECK(toolEntries("plugin.ns.t") == 1);
        editor::EditorSession session;
        REQUIRE(session.perform("plugin.ns.a"));
        CHECK(logged(logs, "acted"));
    }
    CHECK(liveAngelScriptEngines() == liveBefore);
}

TEST_CASE("editing a real script reloads it without leaking engines or registrations") {
    const auto liveBefore = liveAngelScriptEngines();
    Temp temp;
    const auto source = [](std::string_view version, bool extra) {
        std::string text =
            "class T { bool input(apex::Event@ e, apex::View@ v) { apex::log(\"input " + std::string(version) + "\"); return true; }\n"
            "          void visual(uint64 s, apex::Visual@ v) {} }\n"
            "void act() { apex::log(\"action " + std::string(version) + "\"); }\n"
            "void main() { apex::log(\"load " + std::string(version) + "\");\n"
            "  apex::registerTool(\"t\", \"T\", \"d\", \"T\"); apex::registerAction(\"a\", \"A\", \"d\", act);\n";
        if (extra) text += "  apex::registerTool(\"extra\", \"E\", \"d\", \"T\");\n";
        return text + "}\n";
    };
    temp.script("rl", "angelscript", source("v1", true), "rl.as");
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
    host.setScriptEngineFactory(factory());
    host.discover({temp.path});
    host.activate();
    CAPTURE(host.plugins()[0].error);
    REQUIRE(host.plugins()[0].loaded);
    CHECK(liveAngelScriptEngines() == liveBefore + 1);
    CHECK(logged(logs, "load v1"));
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    host.setDocument(&document); host.setSession(&session);
    editor::ToolView view;
    const auto event = press();
    REQUIRE(session.perform("plugin.rl.t"));
    CHECK(session.input(event, view));
    CHECK(logged(logs, "input v1"));
    REQUIRE(toolEntries("plugin.rl.extra") == 1);

    auto now = clock_type::now();
    for (const char* version : {"v2", "v3", "v4"}) {
        logs.clear();
        temp.write("rl", source(version, std::string_view(version) != "v3"), "rl.as");
        CHECK(settle(host, now) == 1);
        CAPTURE(host.plugins()[0].error);
        CHECK(host.plugins()[0].loaded);
        CHECK(liveAngelScriptEngines() == liveBefore + 1);        // old engine freed on every reload
        CHECK(toolEntries("plugin.rl.t") == 1);                   // never a duplicate registration
        CHECK(logged(logs, std::string("load ") + version));
        CHECK(session.input(event, view));                        // the session runs the NEW code
        CHECK(logged(logs, std::string("input ") + version));
        CHECK_FALSE(logged(logs, "input v1"));
        REQUIRE(session.perform("plugin.rl.a"));
        CHECK(logged(logs, std::string("action ") + version));
    }
    CHECK(settle(host, now) == 0);
    host.setSession(nullptr); host.setDocument(nullptr);
}

TEST_CASE("broken real scripts are contained, located, and a fixed save recovers") {
    const auto liveBefore = liveAngelScriptEngines();
    Temp temp;
    const auto good = std::string(
        "class T { bool input(apex::Event@ e, apex::View@ v) { apex::log(\"good input\"); return true; } }\n"
        "void main() { apex::registerTool(\"t\", \"T\", \"d\", \"T\"); }\n");
    temp.script("bad", "angelscript", good, "bad.as");
    temp.script("steady", "angelscript", good, "steady.as");
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
    host.setScriptEngineFactory(factory());
    host.discover({temp.path});
    host.activate();
    REQUIRE(find(host, "bad")->loaded);
    REQUIRE(find(host, "steady")->loaded);
    CHECK(liveAngelScriptEngines() == liveBefore + 2);
    editor::EditorSession session;
    editor::ToolView view;
    const auto event = press();
    auto now = clock_type::now();
    const auto reload = [&](std::string_view body) { logs.clear(); temp.write("bad", body, "bad.as"); return settle(host, now); };
    const auto errorOf = [&] { return find(host, "bad")->error; };

    // Compile error: located (file:line:col), logged, engine freed, other plugin untouched.
    CHECK(reload("void main() {\n  int x = ;\n}\n") == 1);
    CHECK_FALSE(find(host, "bad")->loaded);
    CHECK(errorOf().find("bad.as:2:") != std::string::npos);
    CHECK(logged(logs, "bad.as:2:"));
    CHECK(liveAngelScriptEngines() == liveBefore + 1);
    // Missing entry point, unknown class, registration with the wrong signature.
    CHECK(reload("class T {}\n") == 1);
    CHECK(errorOf().find("main") != std::string::npos);
    CHECK(reload("void main() { apex::registerTool(\"t\", \"T\", \"d\", \"Nope\"); }\n") == 1);
    CHECK_FALSE(find(host, "bad")->loaded);
    CHECK(errorOf().find("Nope") != std::string::npos);
    CHECK(reload("class T { int input(apex::Event@ e, apex::View@ v) { return 1; } }\nvoid main() { apex::registerTool(\"t\", \"T\", \"d\", \"T\"); }\n") == 1);
    CHECK_FALSE(find(host, "bad")->loaded);
    // A runtime exception while loading is contained and located.
    CHECK(reload("void main() {\n  array<int> a;\n  a[3] = 1;\n}\n") == 1);
    CHECK_FALSE(find(host, "bad")->loaded);
    CHECK(errorOf().find("bad.as:3") != std::string::npos);
    CHECK(liveAngelScriptEngines() == liveBefore + 1);
    // The healthy plugin keeps working throughout.
    session.setActiveTool(editor::EditorSession::Tool::Select);
    REQUIRE(session.perform("plugin.steady.t"));
    logs.clear();
    CHECK(session.input(event, view));
    CHECK(logged(logs, "good input"));

    // A fixed save recovers and registers the id exactly once.
    CHECK(reload(good) == 1);
    CHECK(find(host, "bad")->loaded);
    CHECK(find(host, "bad")->error.empty());
    CHECK(toolEntries("plugin.bad.t") == 1);
    CHECK(liveAngelScriptEngines() == liveBefore + 2);
}

TEST_CASE("a script that throws or loops forever in a tool call cannot take the editor down") {
    Temp temp;
    temp.script("hostile", "angelscript",
        "class T {\n"
        "  int mode = 0;\n"
        "  bool input(apex::Event@ e, apex::View@ v) {\n"
        "    if (e.kind == apex::InputKind::Press) { mode++; if (mode == 1) { array<int> a; a[9] = 1; } if (mode == 2) { while (true) { mode++; } } }\n"
        "    return true;\n"
        "  }\n"
        "  void visual(uint64 s, apex::Visual@ v) { v.line(apex::Vec3(0,0,0), apex::Vec3(1,1,1), 1, 1, 1, 1, 1); }\n"
        "}\n"
        "void main() { apex::registerTool(\"t\", \"T\", \"d\", \"T\"); }\n", "hostile.as");
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
    host.setScriptEngineFactory(factory());
    host.discover({temp.path});
    host.activate();
    REQUIRE(host.plugins()[0].loaded);
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    REQUIRE(session.perform("plugin.hostile.t"));
    editor::ToolView view;
    const auto event = press();
    // 1: index out of range -> contained, logged with a location; input is reported as not handled.
    CHECK_NOTHROW(session.input(event, view));
    CHECK(logged(logs, "hostile.as:4"));
    CHECK(logged(logs, "exception"));
    // 2: endless loop -> aborted by the time budget (250 ms) instead of hanging.
    logs.clear();
    const auto start = clock_type::now();
    CHECK_NOTHROW(session.input(event, view));
    const auto elapsed = std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
    CHECK(elapsed < 5000.0);
    CHECK(logged(logs, "exception"));
    // 3: the tool keeps working afterwards and still draws.
    CHECK(session.input(event, view));
    CHECK(session.activeToolObject().overlay(view, 7).worldSegments.size() == 1);
}

TEST_CASE("the stair example loads from its generated folder and commits as one undo step") {
    namespace fs = std::filesystem;
    Temp temp;
    fs::copy(STAIR_PLUGIN_DIR, temp.path / "stair", fs::copy_options::recursive);
    std::vector<std::string> logs;
    Host host(temp.path / "state", temp.path / "breadcrumb", [&](std::string_view s) { logs.emplace_back(s); });
    host.setScriptEngineFactory(factory());
    host.discover({temp.path});
    host.setDocument(nullptr);
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    host.setDocument(&document);
    host.activate();
    REQUIRE(host.plugins().size() == 1);
    CAPTURE(host.plugins()[0].error);
    CAPTURE(logText(logs));
    REQUIRE(host.plugins()[0].loaded);
    editor::EditorSession session;
    session.attach(document);
    host.setSession(&session);
    REQUIRE(session.perform("plugin.stair.tool"));
    editor::ToolView view;
    view.cursorRay = {{100, 100, 64}, {0, 0, -1}};
    auto event = press();
    event.kind = editor::InputKind::Move;
    CHECK_FALSE(session.input(event, view));                           // moving places the preview, handles nothing
    CHECK(session.activeToolObject().preview().ghosts().size() == 8);  // default step count
    const auto readout = session.activeToolObject().readout();
    REQUIRE(readout.parameters.size() == 1);
    CHECK(readout.parameters[0].id == "steps");
    CHECK(session.activeToolObject().setParameter("steps", 5));
    CHECK(session.input(event, view) == false);
    CHECK(session.activeToolObject().preview().ghosts().size() == 5);
    CHECK(session.activeToolObject().setParameter("steps", 500));      // clamped to the declared range
    CHECK_FALSE(session.input(event, view));
    CHECK(session.activeToolObject().preview().ghosts().size() == 64);
    CHECK(session.activeToolObject().setParameter("steps", 5));
    CHECK_FALSE(session.input(event, view));

    const auto count = document.undoCount();
    const auto solidsIn = [&] {
        auto file = document.toVmf();
        REQUIRE(file);
        const auto text = vmf::write(*file);
        int n = 0;
        for (auto at = text.find("solid"); at != std::string::npos; at = text.find("solid", at + 1)) ++n;
        return n;
    };
    const int before = solidsIn();
    event.kind = editor::InputKind::Press;
    CHECK(session.input(event, view));
    CHECK(document.undoCount() == count + 1);                          // one undo step for the whole stair
    CHECK(solidsIn() == before + 5);
    REQUIRE(document.undo());
    CHECK(document.undoCount() == count);
    CHECK(solidsIn() == before);                                       // and one undo removes every step
    host.setSession(nullptr); host.setDocument(nullptr);
}

TEST_CASE("P5 script cost for a 64-step stair (opt-in)") {
    // Set APEX_MEASURE_SCRIPT=1 to run. One input+refresh is: the tool's input() (which runs the script's
    // visual once through refresh()) plus overlay() (which runs it a second time), i.e. TWO full script
    // visual() runs each building 64 six-plane ghosts. Compared against the native StairTool floor.
    if (!std::getenv("APEX_MEASURE_SCRIPT")) return;
    namespace fs = std::filesystem;
    const auto measure = [&](const char* label, const fs::path& dir, std::string_view tool, std::string_view param) {
        Host host(dir / "state", dir / "breadcrumb");
        host.setScriptEngineFactory(factory());
        host.discover({dir});
        host.activate();
        REQUIRE(host.plugins()[0].loaded);
        document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
        editor::EditorSession session;
        session.attach(document);
        REQUIRE(session.perform(std::string(tool)));
        if (!param.empty()) REQUIRE(session.activeToolObject().setParameter(param, 64));
        editor::ToolView view;
        auto event = press();
        event.kind = editor::InputKind::Move;
        constexpr int runs = 300;
        std::vector<double> samples;
        for (int i = -20; i < runs; ++i) {
            view.cursorRay = {{double(i % 50) * 8.0, double(i % 31) * 8.0, 64}, {0, 0, -1}};   // new cursor each time
            const auto start = clock_type::now();
            session.input(event, view);
            session.activeToolObject().overlay(view, 7);
            if (i >= 0) samples.push_back(std::chrono::duration<double, std::milli>(clock_type::now() - start).count());
        }
        std::ranges::sort(samples);
        double sum = 0; for (double v : samples) sum += v;
        const double mean = sum / double(samples.size()), median = samples[samples.size() / 2], p99 = samples[samples.size() * 99 / 100];
        MESSAGE(std::string(label) << ": 64 ghosts, per input+refresh mean " << mean << " ms, median " << median << " ms, p99 " << p99 << " ms");
        CHECK(session.activeToolObject().preview().ghosts().size() == 64);
        return mean;
    };
    Temp floor;
    floor.script("stairs", "fake", "stair s 64\n");
    measure("native floor (fake engine, native tool)", floor.path, "plugin.stairs.s", "");
    Temp real;
    fs::copy(STAIR_PLUGIN_DIR, real.path / "stair", fs::copy_options::recursive);
    const double mean = measure("AngelScript stair", real.path, "plugin.stair.tool", "steps");
#ifdef NDEBUG
    CHECK(mean < 2.0);
#else
    MESSAGE("debug build: the 2 ms budget is only judged in a release build (mean " << mean << " ms)");
#endif
}
#else
TEST_CASE("real AngelScript cases are compiled out when APEX_WITH_ANGELSCRIPT is OFF") {
    CHECK_FALSE(angelScriptCompiledIn());
    CHECK(liveAngelScriptEngines() == 0);
    CHECK_FALSE(static_cast<bool>(defaultScriptEngineFactory()));
}
#endif
