#include <doctest/doctest.h>
#include "editor/compile_snapshot.hpp"
#include "fixtures.hpp"

#include <chrono>
#include <fstream>
#include <iterator>

using namespace apex;
namespace {
namespace fs = std::filesystem;
struct CompileDir {
    fs::path path = fs::temp_directory_path() / ("apex-compile-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    CompileDir() { fs::create_directories(path); }
    ~CompileDir() { std::error_code ignored; fs::remove_all(path, ignored); }
};
std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
}

TEST_CASE("compile snapshot clears outputs and preserves document and source") {
    CompileDir dir;
    document::Document doc(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor_test::addBlock(doc, "entity { id 2 classname info_target }");
    const auto before = doc.write();
    REQUIRE(before);
    const auto revision = doc.historyState();
    const auto undo = doc.undoCount();
    const auto source = dir.path / "source map.vmf";
    std::ofstream(source) << "original source";
    const auto work = dir.path / "cache/compile/source map";
    fs::create_directories(work);
    for (const auto* ext : {".lin", ".prt", ".bsp", ".log"})
        std::ofstream(work / (std::string("source map") + ext)) << "stale";
    editor::CompileGamePaths game{dir.path / "game", dir.path / "bin", dir.path / "instances",
        {{"bsp_exe", "custom vbsp"}, {"vis_exe", "custom vvis"}, {"light_exe", "custom vrad"}}};
    auto snapshot = editor::writeCompileSnapshot(doc, source, dir.path / "cache", game);
    REQUIRE(snapshot);
    CHECK(snapshot->path == work / "source map.vmf");
    CHECK(read(snapshot->path) == *before);
    CHECK(read(source) == "original source");
    const auto after = doc.write();
    REQUIRE(after);
    CHECK(*after == *before);
    CHECK(doc.historyState() == revision);
    CHECK(doc.undoCount() == undo);
    CHECK(snapshot->revision == revision);
    CHECK(std::distance(fs::directory_iterator(work), fs::directory_iterator{}) == 1);
    const auto& paths = snapshot->paths;
    CHECK(paths.workDirectory == work);
    CHECK(paths.bspDirectory == game.gameDirectory / "maps");
    CHECK(paths.toolDirectory == game.toolDirectory);
    CHECK(paths.instanceRoot == game.instanceRoot);
    CHECK(paths.variables.at("path") == work.string());
    CHECK(paths.variables.at("file") == "source map");
    CHECK(paths.variables.at("mapname") == "source map");
    CHECK(paths.variables.at("work") == work.string());
    CHECK(paths.variables.at("addon").empty());
    CHECK(paths.variables.at("bspdir") == (game.gameDirectory / "maps").string());
    CHECK(paths.variables.at("gamedir") == game.gameDirectory.string());
    CHECK(paths.variables.at("bsp_exe") == "custom vbsp");
    CHECK(paths.variables.at("vis_exe") == "custom vvis");
    CHECK(paths.variables.at("light_exe") == "custom vrad");
}

TEST_CASE("compile snapshot supports unsaved maps and refuses to erase a source in cache") {
    CompileDir dir;
    document::Document doc(vmf::emptyMap());
    const auto revision = doc.historyState();
    auto snapshot = editor::writeCompileSnapshot(doc, {}, dir.path, {});
    REQUIRE(snapshot);
    CHECK(snapshot->path == dir.path / "compile/untitled/untitled.vmf");
    CHECK(snapshot->paths.variables.at("file") == "untitled");
    CHECK(doc.historyState() == revision);
    const auto original = read(snapshot->path);
    auto unsafe = editor::writeCompileSnapshot(doc, snapshot->path, dir.path, {});
    CHECK_FALSE(unsafe);
    CHECK(read(snapshot->path) == original);
}

TEST_CASE("compile snapshot copies instances that exist only beside the source") {
    CompileDir dir;
    fs::create_directories(dir.path / "src/inst/deep");
    std::ofstream(dir.path / "src/inst/a.vmf") << "entity { \"classname\" \"func_instance\" \"file\" \"deep/b.vmf\" }";
    std::ofstream(dir.path / "src/inst/deep/b.vmf") << "leaf";
    std::ofstream(dir.path / "src/outside.vmf") << "not referenced";
    document::Document doc(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor_test::addBlock(doc, "entity { id 2 classname func_instance file \"inst/a.vmf\" }");
    editor_test::addBlock(doc, "entity { id 3 classname func_instance file \"../escape.vmf\" }");
    editor_test::addBlock(doc, "entity { id 4 classname func_instance file \"missing/c.vmf\" }");
    std::ofstream(dir.path / "escape.vmf") << "must not be copied";
    auto snapshot = editor::writeCompileSnapshot(doc, dir.path / "src/map.vmf", dir.path / "cache", {});
    REQUIRE(snapshot);
    const auto work = snapshot->paths.workDirectory;
    CHECK(fs::is_regular_file(work / "inst/a.vmf"));
    CHECK(read(work / "inst/deep/b.vmf") == "leaf");
    CHECK_FALSE(fs::exists(work / "outside.vmf"));
    CHECK_FALSE(fs::exists(work / "../escape.vmf"));
    CHECK_FALSE(fs::exists(work / "missing"));
}
