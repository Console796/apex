#include "compile/runner.hpp"
#include "compile/wc_import.hpp"
#include "scripted_runner.hpp"
#include "../assets/fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdlib>

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
