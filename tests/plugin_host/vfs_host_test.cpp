// apex.vfs/1 and apex.assets/1 through the plugin host, against tests/plugin_host/vfs_plugin.cpp.
#include "plugin_host/plugin_host.hpp"
#include "document/document.hpp"
#include "editor/editor_session.hpp"
#include "../assets/fixture.hpp"
#include "../editor/fixtures.hpp"
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace apex;
using namespace apex::plugin_host;
using namespace std::chrono_literals;

namespace {
void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
// Resets the plugin's switches on entry and exit so one test cannot leak into the next.
struct Env {
    Env() { reset(); }
    ~Env() { reset(); }
    static void reset() {
        for (const char* name : {"APEX_TEST_VFS_MODE", "APEX_TEST_VFS_POSITION", "APEX_TEST_VFS_SLEEP_MS",
                                 "APEX_TEST_VFS_MATERIAL", "APEX_TEST_VFS_VALUE"})
            setEnv(name, "");
    }
};
std::string entryFor(std::string_view path) {
#ifdef _WIN32
    constexpr auto platform = "windows-x86_64";
#else
    constexpr auto platform = "linux-x86_64";
#endif
    return "entry = { \"" + std::string(platform) + "\" = \"" + std::string(path) + "\" }";
}
struct Rig {
    Env env;
    GameTree game;
    std::filesystem::path plugins = std::filesystem::temp_directory_path() /
        ("apex-vfs-plugin-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    assets::GameProfile profile;
    JobPool jobs{2};
    std::vector<std::string> logs;
    std::unique_ptr<Host> host;
    Rig() {
        game.put("Game/gameinfo.txt", R"(GameInfo { FileSystem { SearchPaths { game "|gameinfo_path|." game "Last" } } })");
        game.put("Game/materials/shared.vmt", "game");
        game.put("Game/materials/only_game.vmt", "game only");
        game.put("Last/materials/shared.vmt", "last");
        profile.gameDirectory = game.root / "Game";
        std::filesystem::create_directories(plugins / "vfs");
        std::ofstream(plugins / "vfs" / "plugin.kv3")
            << "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n"
            << "{ id = \"vfs\" name = \"Vfs\" version = \"1.0.0\" apex_abi = { major = 1 minor = 0 } " << entryFor(VFS_PLUGIN_PATH) << " }";
    }
    ~Rig() {
        host.reset();
        std::error_code ec;
        std::filesystem::remove_all(plugins, ec);
    }
    void start(std::chrono::milliseconds timeout = 5000ms) {
        host = std::make_unique<Host>(plugins / "state", plugins / "breadcrumb", [this](std::string_view t) { logs.emplace_back(t); });
        host->setVfsTimeout(timeout);
        host->discover({plugins});
        host->activate();
        REQUIRE(host->plugins().size() == 1);
        CAPTURE(host->plugins()[0].error);
        REQUIRE(host->plugins()[0].loaded);
    }
    Result<assets::Vfs> load() { return assets::Vfs::load(profile, jobs, {}, host->vfsProviders()); }
    bool logged(std::string_view text) const { return std::ranges::find(logs, text) != logs.end(); }
    bool loggedPrefix(std::string_view prefix) const { return std::ranges::any_of(logs, [&](const auto& l) { return l.starts_with(prefix); }); }
};
std::string text(const assets::Blob& blob) { return {reinterpret_cast<const char*>(blob.data()), blob.size()}; }
using Clock = std::chrono::steady_clock;
long millis(Clock::time_point since) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count(); }
}

TEST_CASE("vfs: registration is activate-only, namespaced and unique") {
    Rig rig;
    rig.start();
    CHECK(rig.logged("register=0"));
    CHECK(rig.logged("duplicate=1"));      // APEX_ERR_INVALID
    CHECK(rig.logged("foreign=1"));        // another plugin's namespace
    REQUIRE(rig.host->vfsProviders().size() == 1);
    const auto infos = rig.host->vfsProviderInfos();
    REQUIRE(infos.size() == 1);
    CHECK(infos[0].id == "plugin.vfs.files");
    CHECK(infos[0].owner == "vfs");
    CHECK_FALSE(infos[0].faulted);
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    rig.host->setDocument(&document); rig.host->setSession(&session);
    REQUIRE(session.perform("plugin.vfs.late_register"));
    CHECK(rig.logged("late_register=" + std::to_string(APEX_ERR_STATE)));
    CHECK(rig.host->vfsProviders().size() == 1);
    rig.host->setDocument(nullptr); rig.host->setSession(nullptr);
}

TEST_CASE("vfs: a plugin provider mounts in order and earlier mounts win") {
    {
        Rig rig;  // last: fills gaps only
        rig.start();
        auto vfs = rig.load();
        REQUIRE(vfs);
        CHECK(text(*vfs->open("materials/shared.vmt")) == "game");
        CHECK(text(*vfs->open("materials/vfsplugin/glass.vmt")).find("glass") != std::string::npos);
        CHECK(vfs->exists("Materials/VfsPlugin/Glass.vmt"));
        const auto labels = vfs->mountLabels();
        CHECK(labels.back() == "provider:Test files");
        CHECK(vfs->omittedFromCompile() == std::vector<std::string>{"Test files"});
    }
    {
        Rig rig;  // first: overrides the game
        setEnv("APEX_TEST_VFS_POSITION", "first");
        rig.start();
        auto vfs = rig.load();
        REQUIRE(vfs);
        CHECK(text(*vfs->open("materials/shared.vmt")).find("vfsplugin/shared") != std::string::npos);
        CHECK(text(*vfs->open("materials/only_game.vmt")) == "game only");
        CHECK(vfs->mountLabels().front() == "provider:Test files");
    }
    {
        Rig rig;  // before the "Last" mount: loses to the game, beats "Last"
        setEnv("APEX_TEST_VFS_POSITION", "before:/Last");
        rig.start();
        auto vfs = rig.load();
        REQUIRE(vfs);
        const auto labels = vfs->mountLabels();
        REQUIRE(labels.size() >= 3);
        CHECK(labels[labels.size() - 2] == "provider:Test files");
        CHECK(text(*vfs->open("materials/shared.vmt")) == "game");
        const auto walked = vfs->walk("materials", "*.vmt");
        CHECK(std::ranges::find(walked, "materials/vfsplugin/glass.vmt") != walked.end());
    }
}

TEST_CASE("vfs: a throwing provider never breaks the load, a walk or the other mounts") {
    for (const char* mode : {"throw_list", "throw_refresh", "throw_open"}) {
        CAPTURE(mode);
        Rig rig;
        setEnv("APEX_TEST_VFS_MODE", mode);
        rig.start();
        auto vfs = rig.load();
        REQUIRE(vfs);
        CHECK(text(*vfs->open("materials/shared.vmt")) == "game");
        const auto walked = vfs->walk("materials", "*.vmt");
        CHECK(std::ranges::find(walked, "materials/only_game.vmt") != walked.end());
        if (std::string_view(mode) == "throw_open") {
            // Indexed, so it is listed; opening fails with an error and does not throw.
            CHECK(vfs->exists("materials/vfsplugin/glass.vmt"));
            CHECK_FALSE(vfs->open("materials/vfsplugin/glass.vmt"));
        } else {
            CHECK_FALSE(vfs->exists("materials/vfsplugin/glass.vmt"));
            CHECK_FALSE(vfs->notes().empty());
        }
        CHECK_FALSE(rig.host->vfsProviderInfos()[0].faulted);   // an exception is not a timeout
    }
}

TEST_CASE("vfs: a slow provider times out, does not hang the walk, and recovers on reload") {
    Rig rig;
    setEnv("APEX_TEST_VFS_MODE", "slow_open");
    setEnv("APEX_TEST_VFS_SLEEP_MS", "400");
    rig.start(100ms);
    auto vfs = rig.load();
    REQUIRE(vfs);
    auto started = Clock::now();
    auto slow = vfs->open("materials/vfsplugin/glass.vmt");
    CHECK_FALSE(slow);
    CHECK(millis(started) < 350);
    CHECK(rig.host->vfsProviderInfos()[0].faulted);
    // Out of service: calls fail at once, and the walk (served from the index) is not affected.
    started = Clock::now();
    CHECK_FALSE(vfs->open("materials/vfsplugin/glass.vmt"));
    CHECK(vfs->walk("materials", "*.vmt").size() == 3);
    CHECK(millis(started) < 50);
    // Still stuck: a refresh cannot revive it. After the call returns, a refresh does.
    CHECK(vfs->refresh(rig.jobs));
    CHECK(rig.host->vfsProviderInfos()[0].faulted);
    std::this_thread::sleep_for(450ms);
    setEnv("APEX_TEST_VFS_MODE", "");
    REQUIRE(vfs->refresh(rig.jobs));
    CHECK_FALSE(rig.host->vfsProviderInfos()[0].faulted);
    CHECK(vfs->open("materials/vfsplugin/glass.vmt"));
}

TEST_CASE("vfs: a provider that hangs in list leaves the load finite and the other mounts intact") {
    Rig rig;
    setEnv("APEX_TEST_VFS_MODE", "slow_list");
    setEnv("APEX_TEST_VFS_SLEEP_MS", "400");
    rig.start(100ms);
    const auto started = Clock::now();
    auto vfs = rig.load();
    REQUIRE(vfs);
    CHECK(millis(started) < 350);
    CHECK(text(*vfs->open("materials/shared.vmt")) == "game");
    CHECK_FALSE(vfs->exists("materials/vfsplugin/glass.vmt"));
    CHECK_FALSE(vfs->notes().empty());
    std::this_thread::sleep_for(450ms);   // let the stuck call finish before the plugin is unloaded
}

TEST_CASE("vfs: walks never overlap a refresh, and an older Vfs may keep opening while a new one loads") {
    Rig rig;
    rig.start();
    auto vfs = rig.load();
    REQUIRE(vfs);
    const auto expected = vfs->walk("materials", "*.vmt");
    REQUIRE(expected.size() == 3);
    // Contract: no refresh until queued walks finish. Rounds of concurrent walks, then a refresh.
    for (int round = 0; round < 10; ++round) {
        std::vector<std::thread> walkers;
        std::atomic<int> wrong{0};
        for (int i = 0; i < 4; ++i)
            walkers.emplace_back([&, task = vfs->walkTask("materials", "*.vmt")] { if (task() != expected) ++wrong; });
        for (auto& t : walkers) t.join();
        CHECK(wrong == 0);
        REQUIRE(vfs->refresh(rig.jobs));
        CHECK(vfs->walk("materials", "*.vmt") == expected);
    }
    // The provider's own gate: an old Vfs keeps opening (shared) while new loads call refresh/list (exclusive).
    std::atomic<bool> stop{false};
    std::atomic<int> opens{0}, failedOpens{0};
    std::vector<std::thread> openers;
    for (int i = 0; i < 3; ++i)
        openers.emplace_back([&] {
            while (!stop) {
                ++opens;
                if (!vfs->open("materials/vfsplugin/glass.vmt")) ++failedOpens;
            }
        });
    for (int i = 0; i < 15; ++i) {
        auto fresh = rig.load();
        REQUIRE(fresh);
        CHECK(fresh->walk("materials", "*.vmt") == expected);
    }
    stop = true;
    for (auto& t : openers) t.join();
    CHECK(opens > 0);
    CHECK(failedOpens == 0);
}

TEST_CASE("vfs: shutting the host down with a call in flight pins the plugin instead of crashing") {
    Rig rig;
    setEnv("APEX_TEST_VFS_MODE", "slow_open");
    setEnv("APEX_TEST_VFS_SLEEP_MS", "250");
    rig.start(100ms);
    auto vfs = rig.load();
    REQUIRE(vfs);
    CHECK_FALSE(vfs->open("materials/vfsplugin/glass.vmt"));   // times out; the call is still running
    rig.host.reset();                                          // waits for it, then unloads
    CHECK_FALSE(vfs->open("materials/vfsplugin/glass.vmt"));   // the adapter outlives the host, failing cleanly
    CHECK(vfs->exists("materials/vfsplugin/glass.vmt"));
}

TEST_CASE("vfs: request_refresh reaches the handler on the editor thread, for the owner only") {
    Rig rig;
    rig.start();
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    rig.host->setDocument(&document); rig.host->setSession(&session);
    REQUIRE(session.perform("plugin.vfs.refresh"));
    CHECK(rig.logged("refresh=" + std::to_string(APEX_ERR_STATE)));   // no handler yet
    std::vector<std::string> asked;
    const auto thread = std::this_thread::get_id();
    rig.host->setVfsRefreshHandler([&](std::string_view id) { CHECK(std::this_thread::get_id() == thread); asked.emplace_back(id); });
    REQUIRE(session.perform("plugin.vfs.refresh"));
    CHECK(rig.logged("refresh=0"));
    CHECK(asked == std::vector<std::string>{"plugin.vfs.files"});
    REQUIRE(session.perform("plugin.vfs.refresh_other"));
    CHECK(rig.logged("refresh_other=" + std::to_string(APEX_ERR_NOT_FOUND)));
    CHECK(asked.size() == 1);
    rig.host->setDocument(nullptr); rig.host->setSession(nullptr);
}

namespace {
struct FakeAssets : AssetsBackend {
    std::vector<MaterialRecord> records{{"vfsplugin/glass", "LightmappedGeneric", "vfsplugin/glass", "glass", "provider:Test files"},
                                        {"other", "UnlitGeneric", "other", "", "/Game"}};
    std::vector<std::pair<std::string, std::vector<VmtEditRequest>>> writes;
    AssetsWriteResult next{true, false, false, {}};
    void materials(const std::function<bool(const MaterialRecord&)>& visit) override {
        for (const auto& r : records) if (!visit(r)) break;
    }
    std::optional<MaterialRecord> material(std::string_view name) override {
        for (const auto& r : records) if (r.name == name) return r;
        return std::nullopt;
    }
    AssetsWriteResult writeVmt(std::string_view material, const std::vector<VmtEditRequest>& edits) override {
        writes.emplace_back(std::string(material), edits);
        return next;
    }
};
}

TEST_CASE("assets: reads, writes and failures cross the ABI with their status") {
    Rig rig;
    rig.start();
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(document);
    rig.host->setDocument(&document); rig.host->setSession(&session);
    // Without a backend every call reports a state error.
    setEnv("APEX_TEST_VFS_MATERIAL", "vfsplugin/glass");
    REQUIRE(session.perform("plugin.vfs.find"));
    CHECK(rig.logged("find=" + std::to_string(APEX_ERR_STATE) + "::::"));
    FakeAssets backend;
    rig.host->setAssetsBackend(&backend);
    REQUIRE(session.perform("plugin.vfs.find"));
    CHECK(rig.logged("find=0:vfsplugin/glass:vfsplugin/glass:glass:provider:Test files"));
    setEnv("APEX_TEST_VFS_MATERIAL", "missing");
    REQUIRE(session.perform("plugin.vfs.find"));
    CHECK(rig.logged("find=" + std::to_string(APEX_ERR_NOT_FOUND) + "::::"));
    REQUIRE(session.perform("plugin.vfs.list"));
    CHECK(rig.logged("list=0:1/2"));
    // A write passes the edits through; the backend's verdict becomes the status.
    setEnv("APEX_TEST_VFS_MATERIAL", "vfsplugin/glass");
    setEnv("APEX_TEST_VFS_VALUE", "metal");
    REQUIRE(session.perform("plugin.vfs.write"));
    REQUIRE(backend.writes.size() == 1);
    CHECK(backend.writes[0].first == "vfsplugin/glass");
    REQUIRE(backend.writes[0].second.size() == 2);
    CHECK(backend.writes[0].second[0].key == "$surfaceprop");
    CHECK(backend.writes[0].second[0].value == "metal");
    CHECK_FALSE(backend.writes[0].second[0].remove);
    CHECK(backend.writes[0].second[1].remove);
    CHECK(rig.loggedPrefix("write=0:"));
    backend.next = {false, true, false, "comes from a VPK"};
    REQUIRE(session.perform("plugin.vfs.write"));
    CHECK(rig.loggedPrefix("write=" + std::to_string(APEX_ERR_UNSUPPORTED) + ":"));
    backend.next = {false, false, true, "no such material"};
    REQUIRE(session.perform("plugin.vfs.write"));
    CHECK(rig.loggedPrefix("write=" + std::to_string(APEX_ERR_NOT_FOUND) + ":"));
    // Malformed edits never reach the backend.
    const auto before = backend.writes.size();
    REQUIRE(session.perform("plugin.vfs.write_bad"));
    CHECK(rig.logged("write_bad=" + std::to_string(APEX_ERR_INVALID)));
    CHECK(rig.logged("write_none=" + std::to_string(APEX_ERR_INVALID)));
    CHECK(backend.writes.size() == before);
    rig.host->setAssetsBackend(nullptr);
    rig.host->setDocument(nullptr); rig.host->setSession(nullptr);
}
