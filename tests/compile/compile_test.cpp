#include "compile/diagnostics.hpp"
#include "compile/game_launch.hpp"
#include "compile/map_files.hpp"
#include "compile/runner.hpp"
#include "compile/wc_import.hpp"
#include "scripted_runner.hpp"
#include "../assets/fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace apex;
using namespace apex::compile;
using namespace apex::compile::test;

TEST_CASE("compile variables preserve whitespace and expand the full table") {
    Variables variables{
        {"gamedir", "/Steam/Portal 2 Community Edition/p2ce"}, {"path", "/tmp/compile work"},
        {"file", "test map"}, {"bspdir", "/game/maps"}, {"bsp_exe", "/bin/vbsp"},
        {"vis_exe", "/bin/vvis"}, {"light_exe", "/bin/vrad"}, {"game_exe", "/game/p2ce.sh"},
        {"postcompiler_exe", "/bin/postcompiler"}, {"addon", "/game/addons/test"},
        {"mapname", "test map"}, {"work", "/tmp/compile work"}};
    for (const auto& [name, value] : variables) {
        auto expanded = expandArguments("$" + name, variables);
        REQUIRE(expanded);
        CHECK(*expanded == std::vector<std::string>{value});
    }
    auto args = expandArguments(R"(-game $gamedir "$path\$file.vmf" "two words" "" $path\$file.bsp)", variables);
    REQUIRE(args);
    const auto joined = (std::filesystem::path(variables["path"]) / "test map").string();
    CHECK(*args == std::vector<std::string>{"-game", variables["gamedir"], joined + ".vmf", "two words", "", joined + ".bsp"});
    variables["path"] = R"(Z:\home\u\Portal 2 Community Edition)";
    auto windows = expandArguments("$path\\$file", variables);
    REQUIRE(windows);
    CHECK(windows->size() == 1);
    CHECK(windows->front() == variables["path"] + static_cast<char>(std::filesystem::path::preferred_separator) + variables["file"]);
    variables["file"] = "literal \"quote\" $unknown";
    CHECK(expandArguments("$file", variables)->front() == variables["file"]);
    CHECK_FALSE(expandArguments("$unknown", variables));
    CHECK_FALSE(expandArguments("\"unclosed", variables));
    CHECK_FALSE(expandVariables("$unknown", variables));
}

TEST_CASE("compile special commands constrain every operand and cwd") {
    GameTree tree;
    tree.put("work/a", "payload");
    std::filesystem::create_directories(tree.root / "maps");
    std::filesystem::create_directories(tree.root / "addon");
    auto cwd = tree.root / "work";
    const std::array roots{cwd, tree.root / "maps", tree.root / "addon"};
    FilesystemFileOps files;
    auto run = [&](SpecialCommand command, std::vector<std::string> args) {
        return runSpecialCommand(command, args, cwd, roots, files);
    };
    REQUIRE(run(SpecialCommand::Copy, {"a", "../maps/b"}));
    CHECK(files.exists(tree.root / "maps/b"));
    REQUIRE(run(SpecialCommand::Rename, {"../maps/b", "../addon/c"}));
    REQUIRE(run(SpecialCommand::Delete, {"../addon/c"}));
    CHECK_FALSE(files.exists(tree.root / "addon/c"));
    REQUIRE(run(SpecialCommand::CopyIfExists, {"absent", "../maps/absent"}));
    REQUIRE(run(SpecialCommand::CopyIfExists, {"a", "../maps/copied"}));
    CHECK_FALSE(run(SpecialCommand::Copy, {"a", "../escape"}));
    CHECK_FALSE(run(SpecialCommand::CopyIfExists, {"../escape", "a"}));
    CHECK_FALSE(run(SpecialCommand::Delete, {"../../escape"}));
    CHECK_FALSE(run(SpecialCommand::Rename, {"a", "../escape"}));
    CHECK_FALSE(run(SpecialCommand::ChangeDirectory, {".."}));
    CHECK_FALSE(run(SpecialCommand::Copy, {"a"}));
    CHECK_FALSE(run(SpecialCommand::Copy, {"missing", "b"}));
    CHECK_FALSE(run(SpecialCommand::ChangeDirectory, {"missing"}));
    std::error_code ec;
    std::filesystem::create_directory_symlink(tree.root, cwd / "outside", ec);
    if (!ec) CHECK_FALSE(run(SpecialCommand::Copy, {"a", "outside/escape"}));
    REQUIRE(run(SpecialCommand::ChangeDirectory, {"../maps"}));
    CHECK(cwd == tree.root / "maps");
    CHECK_FALSE(files.exists(tree.root / "escape"));
}

namespace {
struct JobFixture {
    GameTree tree;
    FilesystemFileOps files;
    ScriptedRunner runner;
    CompilePaths paths;
    CompileProfile profile;
    JobFixture() {
        tree.put("bin/vbsp", "fake");
        tree.put("bin/vrad", "fake");
        std::filesystem::create_directories(tree.root / "work");
        paths.workDirectory = tree.root / "work";
        paths.bspDirectory = tree.root / "maps";
        paths.toolDirectory = tree.root / "bin";
        paths.instanceRoot = tree.root / "sdk/maps";
        paths.variables = {{"bsp_exe", (tree.root / "bin/vbsp").string()},
                           {"light_exe", (tree.root / "bin/vrad").string()},
                           {"postcompiler_exe", (tree.root / "bin/postcompiler").string()},
                           {"gamedir", "/Steam/Portal 2 Community Edition/p2ce"}};
        profile.name = "test";
        profile.commands = {{"$bsp_exe", "map"}, {"$postcompiler_exe", "map"}, {"$light_exe", "map"}};
    }
};
}

TEST_CASE("compile runner sequences logs skips postcompiler and adds tool context") {
    JobFixture f;
    f.runner.scripts = {{{"vbsp line", "merged stderr"}, 0, false}, {{"vrad line"}, 0, false}};
    CompileJob job(f.profile, f.paths, f.runner, f.files);
    REQUIRE(job.start());
    CHECK(job.status() == Status::Ok);
    REQUIRE(f.runner.calls.size() == 2);
    CHECK(job.steps()[0].lines == std::vector<std::string>{"vbsp line", "merged stderr"});
    CHECK(job.steps()[1].status == Status::Skipped);
    CHECK(job.steps()[1].message.find("postcompiler") != std::string::npos);
    CHECK(job.steps()[2].exitCode == 0);
    CHECK(f.runner.calls[0].args == std::vector<std::string>{"-instancepath", f.paths.instanceRoot.string(),
                                                            "-game", f.paths.variables["gamedir"], "map"});
    CHECK(f.runner.calls[0].cwd == f.paths.workDirectory);
#ifdef __linux__
    CHECK(f.runner.calls[0].env.at("LD_LIBRARY_PATH") == f.paths.toolDirectory.string());
#endif
    CHECK_FALSE(job.start());
}

TEST_CASE("compile preflight checks later tools and variables before any process") {
    JobFixture f;
    SUBCASE("later missing tool") { f.paths.variables["light_exe"] = "/absent/vrad"; }
    SUBCASE("later unknown argument") { f.profile.commands.back().arguments = "$unknown"; }
    SUBCASE("later unknown executable") { f.profile.commands.back().executable = "$unknown"; }
    CompileJob job(f.profile, f.paths, f.runner, f.files);
    auto started = job.start();
    REQUIRE_FALSE(started);
    CHECK(job.status() == Status::Failed);
    CHECK_FALSE(started.error().message.empty());
    CHECK(f.runner.calls.empty());
}

TEST_CASE("compile runner stops on failure and cancels active processes") {
    JobFixture f;
    SUBCASE("nonzero exit") {
        f.runner.scripts = {{{"error"}, 7, false}};
        CompileJob job(f.profile, f.paths, f.runner, f.files);
        REQUIRE(job.start());
        CHECK(job.status() == Status::Failed);
        CHECK(job.steps()[0].status == Status::Failed);
        CHECK(job.steps()[0].exitCode == 7);
        CHECK(job.steps()[2].status == Status::Skipped);
        CHECK(f.runner.calls.size() == 1);
    }
    SUBCASE("cancel") {
        f.runner.scripts = {{{}, 0, true}};
        CompileJob job(f.profile, f.paths, f.runner, f.files);
        REQUIRE(job.start());
        CHECK(job.status() == Status::Running);
        job.cancel();
        CHECK(job.status() == Status::Cancelled);
        CHECK(f.runner.handles[0]->terminated);
        CHECK(job.steps()[0].status == Status::Cancelled);
        CHECK(job.steps()[2].status == Status::Cancelled);
        f.runner.handles[0]->finish();
        CHECK(job.status() == Status::Cancelled);
        CHECK(f.runner.calls.size() == 1);
    }
    SUBCASE("start failure") {
        CompileJob job(f.profile, f.paths, f.runner, f.files);
        REQUIRE(job.start());
        CHECK(job.status() == Status::Failed);
        CHECK(job.steps()[2].status == Status::Skipped);
    }
}

TEST_CASE("compile runner respects explicit flags disabled commands and no wait") {
    JobFixture f;
    f.profile.commands[0].arguments = "-game custom -instancepath explicit";
    f.profile.commands[0].wait = false;
    f.profile.commands[1].enabled = false;
    f.profile.commands[1].arguments = "$unknown";
    f.runner.scripts = {{{}, 0, true}, {{}, 0, false}};
    CompileJob job(f.profile, f.paths, f.runner, f.files);
    REQUIRE(job.start());
    CHECK(job.status() == Status::Ok);
    REQUIRE(f.runner.calls.size() == 2);
    CHECK(f.runner.calls[0].args == std::vector<std::string>{"-game", "custom", "-instancepath", "explicit"});
    CHECK(job.steps()[1].status == Status::Skipped);
}

TEST_CASE("compile WC imports Apex authored text and round trips Strata commands") {
    GameTree tree;
    // Apex-authored synthetic sequence, independent of the installed Hammer files.
    tree.put("synthetic.wc", R"("Command Sequences"
    {
        "Apex test" {
            "1" { "enabled" "1" "run" "$bsp_exe" "params" "-game $gamedir $path\\$file" "special_cmd" "0" "no_wait" "0" }
            "2" { "enabled" "1" "run" "/custom/script" "params" "hello" "special_cmd" "0" "no_wait" "1" }
            "3" { "enabled" "0" "run" "/disabled" "special_cmd" "0" }
            "4" { "enabled" "1" "params" "$work/a $bspdir/a" "special_cmd" "257" }
        }
        "Other" { "1" { "enabled" "1" "run" "$light_exe" "special_cmd" "0" } }
    })");
    auto imported = importWc(tree.root / "synthetic.wc");
    REQUIRE(imported);
    REQUIRE(imported->size() == 2);
    CHECK(imported->at(0).name == "Apex test");
    CHECK(imported->at(1).name == "Other");
    CHECK_FALSE(imported->at(0).commands[1].wait);
    CHECK(listNonToolExecutables(imported->at(0)) == std::vector<std::string>{"/custom/script", "Copy File"});
    for (auto special : {SpecialCommand::ChangeDirectory, SpecialCommand::Delete, SpecialCommand::Rename, SpecialCommand::CopyIfExists})
        imported->at(0).commands.push_back({"", "a b", true, true, special});
    tree.put("roundtrip.wc", exportWc(imported->at(0)));
    auto roundtrip = importWc(tree.root / "roundtrip.wc");
    REQUIRE(roundtrip);
    CHECK(roundtrip->at(0) == imported->at(0));
    CHECK_FALSE(importWc(tree.root / "missing.wc"));
    tree.put("invalid.wc", "invalid");
    CHECK_FALSE(importWc(tree.root / "invalid.wc"));
}

TEST_CASE("compile builtins and installed Hammer Fast have six step shape") {
    auto check = [](const CompileProfile& profile) {
        REQUIRE(profile.commands.size() == 6);
        CHECK(profile.commands[0].executable == "$bsp_exe");
        CHECK(profile.commands[1].executable == "$postcompiler_exe");
        CHECK(profile.commands[2].executable == "$vis_exe");
        CHECK(profile.commands[3].executable == "$light_exe");
        CHECK(profile.commands[4].special == SpecialCommand::Copy);
        CHECK(profile.commands[5].executable == "$game_exe");
        CHECK_FALSE(profile.commands[5].wait);
    };
    const auto profiles = builtinProfiles();
    REQUIRE(profiles.size() == 3);
    CHECK(profiles[0].name == "Fast");
    CHECK(profiles[1].name == "Normal");
    CHECK(profiles[2].name == "Final");
    for (const auto& profile : profiles) check(profile);
    CHECK(profiles[0].commands[2].arguments.find("-fast") != std::string::npos);
    CHECK(profiles[1].commands[2].arguments.find("-fast") == std::string::npos);
    CHECK(profiles[2].commands[3].arguments.find("-hdr") != std::string::npos);
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped corpus: APEX_P2CE_DIR is unset"); return; }
    for (const auto* file : {"cmdseq.wc", "CmdSeqDefault.wc"}) {
        const auto path = std::filesystem::path(install) / "hammer/cfg" / file;
        CAPTURE(path.string());
        auto imported = importWc(path);
        REQUIRE(imported);
        const auto fast = std::ranges::find(*imported, "Fast", &CompileProfile::name);
        REQUIRE(fast != imported->end());
        check(*fast);
    }
}

TEST_CASE("compile runner resumes asynchronous steps and runs specials in job cwd") {
    JobFixture f;
    f.tree.put("work/input", "data");
    std::filesystem::create_directories(f.paths.workDirectory / "sub");
    f.profile.commands = {
        {"", "sub", true, true, SpecialCommand::ChangeDirectory},
        {"$bsp_exe", "map"},
        {"", "../input output", true, true, SpecialCommand::Copy},
        {"$light_exe", "map"},
    };
    f.runner.scripts = {{{"first"}, 0, true}, {{"second"}, 0, true}};
    CompileJob job(f.profile, f.paths, f.runner, f.files);
    REQUIRE(job.start());
    CHECK(job.status() == Status::Running);
    REQUIRE(f.runner.calls.size() == 1);
    CHECK(f.runner.calls[0].cwd == f.paths.workDirectory / "sub");
    f.runner.handles[0]->finish();
    CHECK(job.status() == Status::Running);
    CHECK(f.files.exists(f.paths.workDirectory / "sub/output"));
    REQUIRE(f.runner.calls.size() == 2);
    f.runner.handles[1]->finish();
    CHECK(job.status() == Status::Ok);
    for (const auto& step : job.steps()) CHECK(step.status == Status::Ok);
}

TEST_CASE("compile profile options suppress game and BSP copy before preflight") {
    JobFixture f;
    f.profile.runGameAfter = false;
    f.profile.copyBspToMaps = false;
    f.profile.commands = {
        {"", "$unknown $bspdir\\$file.bsp", true, true, SpecialCommand::Copy},
        {"$game_exe", "$unknown"},
        {"$bsp_exe", "map"},
    };
    f.runner.scripts = {{{}, 0, false}};
    CompileJob job(f.profile, f.paths, f.runner, f.files);
    REQUIRE(job.start());
    CHECK(job.status() == Status::Ok);
    CHECK(job.steps()[0].status == Status::Skipped);
    CHECK(job.steps()[1].status == Status::Skipped);
    CHECK(f.runner.calls.size() == 1);
}

TEST_CASE("compile explicit game flag needs no default game directory") {
    JobFixture f;
    f.paths.variables.erase("gamedir");
    f.profile.commands = {{"$bsp_exe", "-game explicit -instancepath explicit map"}};
    f.runner.scripts = {{{}, 0, false}};
    CompileJob job(f.profile, f.paths, f.runner, f.files);
    REQUIRE(job.start());
    CHECK(job.status() == Status::Ok);
    REQUIRE(f.runner.calls.size() == 1);
    CHECK(f.runner.calls[0].args == std::vector<std::string>{"-game", "explicit", "-instancepath", "explicit", "map"});
}

namespace {
// Logs the real P2CE Linux tools wrote for broken variants of the D17 chamber (tools/capture_compile_logs.py).
std::vector<std::string> logLines(const std::string& name) {
    std::ifstream in(std::filesystem::path(APEX_COMPILE_LOG_DIR) / name);
    REQUIRE_MESSAGE(in, name);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}
}

TEST_CASE("compile diagnostics: one golden test per rule over captured logs") {
    const auto only = [](const std::string& tool, const std::string& log) {
        auto found = parseLog(tool, logLines(log));
        REQUIRE(found.size() == 1);
        return found.front();
    };
    SUBCASE("vbsp.brush maps to the brush id and keeps side and texture") {
        const auto d = only("vbsp", "invalid_brush.vbsp.log");
        CHECK(d.rule == "vbsp.brush");
        CHECK(d.severity == Severity::Error);
        CHECK(d.objects == std::vector{ObjectRef{ObjectKind::Brush, 1}});
        CHECK(d.text == "Brush 1: FloatPlane: bad normal, side 0, texture tile/white_wall_tile003a");
    }
    SUBCASE("vbsp.material") {
        // The chamber's baseline already lacks tile/black_wall_metal_002a; the variant adds one more.
        auto found = parseLog("vbsp", logLines("unknown_material.vbsp.log"));
        REQUIRE(found.size() == 2);
        CHECK(found[0].rule == "vbsp.material");
        CHECK(found[0].severity == Severity::Warning);
        CHECK(found[0].text == "Material not found: apex/not_a_material");
        CHECK(found[0].objects.empty());
        CHECK(found[1].text == "Material not found: tile/black_wall_metal_002a");
    }
    SUBCASE("vbsp.model") {
        auto found = parseLog("vbsp", logLines("missing_model.vbsp.log"));
        REQUIRE(found.size() == 2);
        CHECK(found[1].rule == "vbsp.model");
        CHECK(found[1].text == "Model failed to load: models/apex/does_not_exist.mdl");
    }
    SUBCASE("vbsp.leak carries the entity position and no object") {
        auto found = parseLog("vbsp", logLines("leak.vbsp.log"));
        REQUIRE(found.size() == 2);
        CHECK(found[1].rule == "vbsp.leak");
        CHECK(found[1].severity == Severity::Error);
        CHECK(found[1].text == "The map leaks from light at (256.00 192.00 8.00)");
        CHECK(found[1].position == std::array{256.0, 192.0, 8.0});
        CHECK(found[1].objects.empty());
    }
    SUBCASE("vbsp.instance") {
        auto found = parseLog("vbsp", logLines("missing_instance.vbsp.log"));
        REQUIRE(found.size() == 2);
        CHECK(found[1].rule == "vbsp.instance");
        CHECK(found[1].text == "Could not open instance file apex_nowhere.vmf");
    }
    SUBCASE("vvis.portals") {
        const auto d = only("vvis", "leak.vvis.log");
        CHECK(d.rule == "vvis.portals");
        CHECK(d.severity == Severity::Error);
    }
    SUBCASE("a clean compile and tool-silent problems produce nothing beyond the known missing material") {
        for (const auto* name : {"baseline", "missing_target", "duplicate_targetname"}) {
            const auto found = parseLog("vbsp", logLines(std::string(name) + ".vbsp.log"));
            REQUIRE(found.size() == 1);
            CHECK(found[0].rule == "vbsp.material");
            CHECK(parseLog("vvis", logLines(std::string(name) + ".vvis.log")).empty());
            CHECK(parseLog("vrad", logLines(std::string(name) + ".vrad.log")).empty());
        }
    }
    SUBCASE("rules apply only to their own tool") {
        CHECK(parseLog("vrad", logLines("invalid_brush.vbsp.log")).empty());
    }
}

TEST_CASE("compile diagnostics: steps, tool names and summary") {
    CHECK(toolName("/x/bin/linux64/vbsp") == "vbsp");
    std::vector<StepResult> steps(3);
    steps[0].program = "/b/vbsp";
    steps[0].lines = logLines("leak.vbsp.log");
    steps[1].program = "/b/vvis";
    steps[1].lines = logLines("leak.vvis.log");
    steps[2].lines = {"Brush 5: not a tool"}; // a special command has no program: never parsed
    const auto all = collectDiagnostics(steps);
    REQUIRE(all.size() == 3);
    CHECK(all[0].step == 0);
    CHECK(all[2].step == 1);
    const auto summary = summarize(all, steps);
    CHECK(summary.tools.at("vbsp").errors == 1);
    CHECK(summary.tools.at("vbsp").warnings == 1);
    CHECK(summary.tools.at("vvis").errors == 1);
    CHECK(summary.seconds == 0);
    steps[1].lines.push_back("12 seconds elapsed");
    CHECK(summarize(all, steps).seconds == 12);
}

TEST_CASE("leak and portal readers read the files the real tools wrote") {
    const std::filesystem::path logs = APEX_COMPILE_LOG_DIR;
    const auto leak = readLeakFile(logs, "leak");
    REQUIRE(leak);
    REQUIRE(leak->size() == 3);
    CHECK((*leak)[0] == DVec3(2048, 2048, -16));
    CHECK((*leak)[1].x == doctest::Approx(315.909851));
    CHECK((*leak)[2] == DVec3(256, 192, 8));
    CHECK_FALSE(readLeakFile(logs, "baseline")); // a compile that did not leak writes no .lin

    const auto portals = readPortalFile(logs, "baseline");
    REQUIRE(portals);
    CHECK(portals->clusters == 29);
    REQUIRE(portals->portals.size() == 75);
    const auto& first = portals->portals[0];
    CHECK(first.clusterA == 0);
    CHECK(first.clusterB == 1);
    CHECK(first.points == std::vector<DVec3>{{208, 400, 0}, {304, 400, 0}, {304, 400, 128}, {208, 400, 128}});
    CHECK(portals->portals[2].points.size() == 9);
    for (const auto& portal : portals->portals) CHECK(portal.points.size() >= 3);
}

TEST_CASE("leak and portal readers tolerate empty and truncated files") {
    CHECK(parseLeakFile("").empty());
    CHECK(parseLeakFile("1 2 3\n4 5").size() == 1);
    CHECK(parseLeakFile("1 2 3\r\n4 5 6\r\n").size() == 2);
    CHECK(parseLeakFile("x y z\n1 2 3\n").empty());
    CHECK(parsePortalFile("").portals.empty());
    CHECK(parsePortalFile("PRT2\n1\n1\n3 0 1 (0 0 0 ) (1 0 0 ) (1 1 0 ) \n").portals.empty());
    CHECK(parsePortalFile("PRT1\n2\n").portals.empty());
    const std::string good = "PRT1\n2\n2\n3 0 1 (0 0 0 ) (1 0 0 ) (1 1 0 ) \n3 1 0 (0 0 0 ) (0 1 0 ) (0 1 1 ) \n";
    const auto full = parsePortalFile(good);
    CHECK(full.clusters == 2);
    CHECK(full.portals.size() == 2);
    // Cut anywhere: only whole portals before the cut survive and nothing throws.
    for (std::size_t length = 0; length < good.size(); ++length) {
        const auto cut = parsePortalFile(good.substr(0, length));
        CHECK(cut.portals.size() <= 2);
        for (const auto& portal : cut.portals) CHECK(portal.points.size() == 3);
    }
    CHECK(parsePortalFile(good.substr(0, good.find("3 1 0") + 12)).portals.size() == 1);
}

TEST_CASE("game launch builds literal arguments for names with spaces and an addon target") {
    GameLaunchRequest request;
    request.gameExe = "/Steam/Portal 2 Community Edition/p2ce.sh";
    request.gameDir = "/Steam/Portal 2 Community Edition/p2ce";
    request.mapName = "my map 'one'";
    auto launch = buildGameLaunch(request);
    REQUIRE(launch);
    CHECK(launch->program == request.gameExe.string());
    CHECK(launch->arguments == std::vector<std::string>{"-dev", "-console", "-allowdebug", "-game",
        "/Steam/Portal 2 Community Edition/p2ce", "+map", "my map 'one'", "-condebug"});
    CHECK(launch->logFile == std::filesystem::path("/Steam/Portal 2 Community Edition/p2ce/console.log"));
    CHECK(launch->bspFile == std::filesystem::path("/Steam/Portal 2 Community Edition/p2ce/maps/my map 'one'.bsp"));

    // A map in an addon is loaded by name through the addon's mount; a campaign adds its own flag.
    request.mapName = "addon_chamber";
    request.campaign = "workshop/my campaign";
    request.arguments = "-game $gamedir -condebug +map $mapname";
    launch = buildGameLaunch(request);
    REQUIRE(launch);
    CHECK(launch->arguments == std::vector<std::string>{"-game", "/Steam/Portal 2 Community Edition/p2ce",
        "-condebug", "+map", "addon_chamber", "-campaign", "workshop/my campaign"});
}

TEST_CASE("game launch rejects what cannot be launched") {
    GameLaunchRequest request;
    request.gameExe = "/g/p2ce.sh";
    request.gameDir = "/g/p2ce";
    request.mapName = "a";
    CHECK(buildGameLaunch(request));
    SUBCASE("no executable") { request.gameExe.clear(); }
    SUBCASE("no game directory") { request.gameDir.clear(); }
    SUBCASE("no map") { request.mapName.clear(); }
    SUBCASE("path in the map name") { request.mapName = "../maps/a"; }
    SUBCASE("unknown variable") { request.arguments = "+map $nothing"; }
    CHECK_FALSE(buildGameLaunch(request));
    CHECK(withConsoleLog({"-condebug"}) == std::vector<std::string>{"-condebug"});
}

TEST_CASE("compile game step starts detached and ignores a game that exits at once") {
    JobFixture f;
    f.tree.put("p2ce.sh", "fake");
    f.paths.variables["game_exe"] = (f.tree.root / "p2ce.sh").string();
    f.paths.variables["file"] = "chamber";
    f.profile.commands = {{"$bsp_exe", "map"}, {"$game_exe", "-game $gamedir +map $file"}};
    f.profile.runGameAfter = true;
    f.runner.scripts = {{{"vbsp line"}, 0, false}};
    CompileJob job(f.profile, f.paths, f.runner, f.files);
    REQUIRE(job.start());
    CHECK(job.status() == Status::Ok);
    CHECK(job.steps()[1].status == Status::Ok);
    CHECK(f.runner.calls.size() == 1);
    REQUIRE(f.runner.detached.size() == 1);
    CHECK(f.runner.detached[0].program == (f.tree.root / "p2ce.sh").string());
    CHECK(f.runner.detached[0].args == std::vector<std::string>{"-game", f.paths.variables["gamedir"], "+map", "chamber", "-condebug"});
    // Cancelling after the sequence is done never reaches the game.
    job.cancel();
    CHECK(job.status() == Status::Ok);
}

TEST_CASE("compile game step failing to start fails the job") {
    JobFixture f;
    f.tree.put("p2ce.sh", "fake");
    f.paths.variables["game_exe"] = (f.tree.root / "p2ce.sh").string();
    f.paths.variables["file"] = "chamber";
    f.profile.commands = {{"$game_exe", "+map $file"}};
    f.profile.runGameAfter = true;
    f.runner.failDetached = true;
    CompileJob job(f.profile, f.paths, f.runner, f.files);
    REQUIRE(job.start());
    CHECK(job.status() == Status::Failed);
    CHECK(job.steps()[0].status == Status::Failed);
}
