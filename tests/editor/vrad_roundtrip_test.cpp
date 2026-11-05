#include <doctest/doctest.h>
#include "../compile/scripted_runner.hpp"
#include "editor/vrad_roundtrip.hpp"
#include "fixtures.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

using namespace apex;
namespace {
namespace fs = std::filesystem;
using compile::test::Script;
using compile::test::ScriptedRunner;

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

struct Sandbox {
    fs::path path = fs::temp_directory_path() / ("apex-vrad-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Sandbox() {
        fs::create_directories(path / "game");
        fs::create_directories(path / "bin");
        for (const auto* tool : {"vbsp", "vvis", "vrad"}) std::ofstream(path / "bin" / tool) << "tool";
    }
    ~Sandbox() { std::error_code ignored; fs::remove_all(path, ignored); }
    editor::CompileGamePaths game() const {
        return {path / "game", path / "bin", path / "instances",
                {{"bsp_exe", (path / "bin/vbsp").string()}, {"vis_exe", (path / "bin/vvis").string()},
                 {"light_exe", (path / "bin/vrad").string()}}};
    }
    fs::path temp() const { return path / "temp"; }
};

std::unique_ptr<document::Document> makeDocument() {
    auto doc = std::make_unique<document::Document>(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor_test::addBox(*doc, {0, 0, 0}, {64, 64, 64});
    return doc;
}
bool contains(const std::vector<std::string>& args, const std::string& text) {
    return std::ranges::find(args, text) != args.end();
}
}

TEST_CASE("vrad round trip runs vbsp, vvis -fast and vrad in a fresh temp directory") {
    Sandbox sandbox;
    auto docPtr = makeDocument();
    auto& doc = *docPtr;
    editor_test::addBlock(doc, R"(entity { id 2 classname light_rt origin "1 2 3" })");
    const auto before = doc.write();
    REQUIRE(before);
    const auto revision = doc.historyState();
    ScriptedRunner runner;
    runner.scripts = {Script{}, Script{}, Script{}};
    compile::FilesystemFileOps files;
    editor::VradRoundTrip trip(runner, files, sandbox.temp());
    REQUIRE(trip.start(doc, sandbox.path / "maps/room.vmf", sandbox.game(), {}));
    REQUIRE(runner.calls.size() == 3);
    CHECK(runner.calls[0].program == (sandbox.path / "bin/vbsp").string());
    CHECK(contains(runner.calls[1].args, "-fast"));
    CHECK(runner.calls[1].program == (sandbox.path / "bin/vvis").string());
    CHECK(contains(runner.calls[2].args, "-noextra"));
    CHECK(runner.calls[2].program == (sandbox.path / "bin/vrad").string());
    CHECK(trip.state() == editor::VradRoundTrip::State::Done);
    // Everything lives under the system-temp root passed in, never under the game or the map folder.
    const auto relative = trip.directory().lexically_relative(sandbox.temp());
    CHECK((!relative.empty() && *relative.begin() != ".."));
    for (const auto& call : runner.calls) CHECK(call.cwd.lexically_relative(sandbox.temp()).begin()->string() != "..");
    CHECK(fs::exists(trip.directory() / "compile/room/room.vmf"));
    // A round trip bakes runtime lights into the exported copy; the document keeps the runtime class.
    auto exported = kv1::parse(read(trip.directory() / "compile/room/room.vmf"));
    REQUIRE(exported);
    bool baked = false;
    for (const auto* entity : exported->root().findAll("entity")) {
        const auto* classname = entity->find("classname");
        REQUIRE(classname);
        CHECK(classname->decodedValue() != "light_rt");
        if (classname->decodedValue() == "light") baked = true;
    }
    CHECK(baked);
    CHECK(trip.bsp() == trip.directory() / "compile/room/room.bsp");
    CHECK(fs::is_empty(sandbox.path / "game"));
    CHECK(*doc.write() == *before);
    CHECK(doc.historyState() == revision);
    CHECK_FALSE(trip.stale(doc.historyState()));
}

TEST_CASE("vrad round trip honours the vis toggle and the extra flags") {
    Sandbox sandbox;
    auto docPtr = makeDocument();
    auto& doc = *docPtr;
    ScriptedRunner runner;
    runner.scripts = {Script{}, Script{}};
    compile::FilesystemFileOps files;
    editor::VradRoundTrip trip(runner, files, sandbox.temp());
    REQUIRE(trip.start(doc, {}, sandbox.game(), {.flags = "-bounce 3 -noextra", .vis = false}));
    REQUIRE(runner.calls.size() == 2);
    CHECK(runner.calls[1].program == (sandbox.path / "bin/vrad").string());
    CHECK(contains(runner.calls[1].args, "-bounce"));
    CHECK(contains(runner.calls[1].args, "3"));
    CHECK(trip.state() == editor::VradRoundTrip::State::Done);
}

TEST_CASE("vrad round trip allows one run at a time and cancels") {
    Sandbox sandbox;
    auto docPtr = makeDocument();
    auto& doc = *docPtr;
    ScriptedRunner runner;
    runner.scripts = {Script{.pending = true}};
    compile::FilesystemFileOps files;
    editor::VradRoundTrip trip(runner, files, sandbox.temp());
    REQUIRE(trip.start(doc, {}, sandbox.game(), {}));
    CHECK(trip.running());
    const auto directory = trip.directory();
    CHECK_FALSE(trip.start(doc, {}, sandbox.game(), {}));
    CHECK(runner.calls.size() == 1);
    trip.cancel();
    CHECK(runner.handles[0]->terminated);
    CHECK(trip.state() == editor::VradRoundTrip::State::Cancelled);
    CHECK(trip.poll() == editor::VradRoundTrip::State::Cancelled);
    // A new run starts afterwards in a new directory and removes the old one.
    runner.scripts = {Script{}, Script{}, Script{}};
    REQUIRE(trip.start(doc, {}, sandbox.game(), {}));
    CHECK(trip.state() == editor::VradRoundTrip::State::Done);
    CHECK(trip.directory() != directory);
    CHECK_FALSE(fs::exists(directory));
}

TEST_CASE("vrad round trip reports a failing tool with its output") {
    Sandbox sandbox;
    auto docPtr = makeDocument();
    auto& doc = *docPtr;
    ScriptedRunner runner;
    runner.scripts = {Script{}, Script{.lines = {"fatal: ran out of luxels"}, .code = 1}};
    compile::FilesystemFileOps files;
    editor::VradRoundTrip trip(runner, files, sandbox.temp());
    REQUIRE(trip.start(doc, {}, sandbox.game(), {}));
    CHECK(trip.state() == editor::VradRoundTrip::State::Failed);
    CHECK(trip.message().find("ran out of luxels") != std::string::npos);
    CHECK(runner.calls.size() == 2);
}

TEST_CASE("an edit during or after a run makes the result stale, and the temp directory is removed") {
    Sandbox sandbox;
    auto docPtr = makeDocument();
    auto& doc = *docPtr;
    ScriptedRunner runner;
    runner.scripts = {Script{.pending = true}};
    compile::FilesystemFileOps files;
    fs::path directory;
    {
        editor::VradRoundTrip trip(runner, files, sandbox.temp());
        CHECK_FALSE(trip.stale(doc.historyState()));
        REQUIRE(trip.start(doc, {}, sandbox.game(), {}));
        directory = trip.directory();
        CHECK(fs::exists(directory));
        CHECK_FALSE(trip.stale(doc.historyState()));
        editor_test::addBox(doc, {128, 0, 0}, {192, 64, 64});
        CHECK(trip.stale(doc.historyState()));
        CHECK(trip.running());
        runner.scripts = {};
        runner.handles[0]->finish();
        CHECK(trip.poll() != editor::VradRoundTrip::State::Running);
    }
    CHECK_FALSE(fs::exists(directory));
}
