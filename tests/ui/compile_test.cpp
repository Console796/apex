#include "test_app.hpp"
#include <doctest/doctest.h>
#include "../editor/fixtures.hpp"
#include "../compile/scripted_runner.hpp"
#include "ui/compile_panel.hpp"
#include "ui/game_content.hpp"
#include "ui/profile_editor.hpp"
#include "ui/profile_store.hpp"
#include <QComboBox>
#include <QSettings>
#include "../editor/corpus.hpp"
#include "editor/compile_snapshot.hpp"

#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>
#include <fstream>
#include <cstdlib>
#ifdef __linux__
#include <csignal>
#include <unistd.h>
#endif

using namespace apex;
namespace {
bool until(const std::function<bool()>& condition, int timeout = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout) QTest::qWait(10);
    return condition();
}
}

#ifdef __linux__
TEST_CASE("compile_chamber Fast profile through real QProcessRunner" *
          doctest::skip([] {
              const auto* install = std::getenv("APEX_P2CE_DIR");
              const auto* chamber = std::getenv("APEX_CHAMBER_VMF");
              return !install || !*install || !chamber || !*chamber;
          }())) {
    ui_test::app();
    const auto install = std::filesystem::canonical(std::getenv("APEX_P2CE_DIR"));
    const auto source = std::filesystem::canonical(std::getenv("APEX_CHAMBER_VMF"));
    assets::GameProfile game;
    game.gameDirectory = install / "p2ce";
    REQUIRE(std::filesystem::is_regular_file(game.gameDirectory / "gameinfo.txt"));
    editor::CompileGamePaths paths{game.gameDirectory, game.toolDirectory(), game.instanceSearchRoot(), {}};
    for (const auto& [variable, tool] : {std::pair{"bsp_exe", "vbsp"}, {"vis_exe", "vvis"},
             {"light_exe", "vrad"}, {"postcompiler_exe", "postcompiler"}, {"game_exe", "game"}})
        paths.tools[variable] = game.toolPath(tool).string();
    QTemporaryDir directory(QStringLiteral("/tmp/apex-compile-chamber-XXXXXX"));
    REQUIRE(directory.isValid());
    const auto root = std::filesystem::canonical(directory.path().toStdString());
    const auto outsideInstall = [&](const std::filesystem::path& path) {
        const auto relative = std::filesystem::weakly_canonical(path).lexically_relative(install);
        return !relative.empty() && *relative.begin() == "..";
    };
    REQUIRE(outsideInstall(root));
    document::Document document(editor_test::readCorpus(source));
    auto snapshot = editor::writeCompileSnapshot(document, source, root, paths);
    REQUIRE_MESSAGE(snapshot, (snapshot ? "" : snapshot.error().what()));
    REQUIRE(outsideInstall(snapshot->paths.workDirectory));
    snapshot->paths.bspDirectory = root / "maps";
    REQUIRE(outsideInstall(snapshot->paths.bspDirectory));
    std::filesystem::create_directory(snapshot->paths.bspDirectory);
    snapshot->paths.variables["bspdir"] = snapshot->paths.bspDirectory.string();
    auto profiles = compile::builtinProfiles();
    auto profile = profiles.front();
    REQUIRE(profile.name == "Fast");
    profile.runGameAfter = false;
    ui::QProcessRunner runner;
    compile::FilesystemFileOps files;
    compile::CompileJob job(profile, snapshot->paths, runner, files);
    auto started = job.start();
    REQUIRE_MESSAGE(started, (started ? "" : started.error().what()));
    REQUIRE(until([&] { return job.status() != compile::Status::Running; }, 600000));
    for (std::size_t i = 0; i < job.steps().size(); ++i) {
        const auto& step = job.steps()[i];
        CAPTURE(i);
        CAPTURE(step.message);
        for (const auto& line : step.lines) {
            INFO(line);
            CHECK(line.find("LEAKED") == std::string::npos);
        }
        if (profile.commands[i].executable == "$postcompiler_exe" ||
            profile.commands[i].executable == "$game_exe")
            CHECK(step.status == compile::Status::Skipped);
        else
            CHECK(step.status == compile::Status::Ok);
    }
    CHECK(job.status() == compile::Status::Ok);
    const auto bsp = source.stem().string() + ".bsp";
    REQUIRE(std::filesystem::is_regular_file(snapshot->paths.workDirectory / bsp));
    REQUIRE(std::filesystem::is_regular_file(snapshot->paths.bspDirectory / bsp));
    CHECK(std::filesystem::file_size(snapshot->paths.workDirectory / bsp) ==
          std::filesystem::file_size(snapshot->paths.bspDirectory / bsp));
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        CHECK(entry.path().extension() != ".lin");
}
#endif

#ifdef __linux__
TEST_CASE("compile QProcess runner merges output, inherits environment and replays completion") {
    ui_test::app();
    QTemporaryDir dir;
    ui::QProcessRunner runner;
    auto started = runner.start("/bin/sh", {"-c", "printf 'out\\n'; printf 'err\\n' >&2; printf '%s' \"$APEX_COMPILE_TEST\"; exit 7"},
        dir.path().toStdString(), {{"APEX_COMPILE_TEST", "tail"}});
    REQUIRE(started);
    auto& handle = *started;
    REQUIRE(until([&] { return handle->exitCode().has_value(); }));
    std::vector<std::string> lines;
    int code = 0;
    int completions = 0;
    handle->setCallbacks([&](std::string line) { lines.push_back(std::move(line)); },
        [&](int value) { code = value; ++completions; });
    CHECK(lines == std::vector<std::string>{"out", "err", "tail"});
    CHECK(code == 7);
    CHECK(completions == 1);
    handle->setCallbacks({}, {});
    handle->setCallbacks([&](std::string) { FAIL("output replayed twice"); }, [&](int) { ++completions; });
    CHECK(completions == 1);
    auto missing = runner.start((dir.path() + "/missing").toStdString(), {}, dir.path().toStdString(), {});
    CHECK_FALSE(missing);
}

TEST_CASE("compile QProcess runner disconnects queued output and survives handle destruction") {
    ui_test::app();
    QTemporaryDir dir;
    ui::QProcessRunner runner;
    auto started = runner.start("/bin/sh", {"-c", "sleep 0.1; echo later"}, dir.path().toStdString(), {});
    REQUIRE(started);
    bool called = false;
    (*started)->setCallbacks([&](std::string) { called = true; }, [&](int) { called = true; });
    (*started)->setCallbacks({}, {});
    REQUIRE(until([&] { return (*started)->exitCode().has_value(); }));
    CHECK_FALSE(called);
    std::vector<std::string> lines;
    (*started)->setCallbacks([&](std::string line) { lines.push_back(std::move(line)); }, {});
    CHECK(lines == std::vector<std::string>{"later"});
    auto detached = runner.start("/bin/sh", {"-c", "sleep 0.1; echo done > detached"}, dir.path().toStdString(), {});
    REQUIRE(detached);
    detached->reset();
    REQUIRE(until([&] { return std::filesystem::exists(dir.path().toStdString() + "/detached"); }));
}

TEST_CASE("compile QProcess runner terminates grandchildren including SIGTERM resistant children") {
    ui_test::app();
    QTemporaryDir dir;
    ui::QProcessRunner runner;
    std::string script;
    SUBCASE("ordinary child") { script = "sleep 300 & echo $!; wait"; }
    SUBCASE("TERM resistant child") {
        script = "sh -c 'trap \"\" TERM; echo $$; exec sleep 300' & wait";
    }
    auto started = runner.start("/bin/sh", {"-c", script},
        dir.path().toStdString(), {});
    REQUIRE(started);
    pid_t child = 0;
    (*started)->setCallbacks([&](std::string line) { child = static_cast<pid_t>(std::stoi(line)); }, {});
    REQUIRE(until([&] { return child > 0; }));
    (*started)->terminate();
    started->reset(); // Escalation must outlive the handle and the original shell.
    const bool stopped = until([&] {
        if (::kill(child, 0) < 0) return true;
        // A dead orphan may wait for the host's init to reap it.
        std::ifstream status("/proc/" + std::to_string(child) + "/stat");
        std::string pid, name;
        char state = 0;
        status >> pid >> name >> state;
        return state == 'Z';
    });
    if (!stopped) ::kill(child, SIGKILL);
    CHECK(stopped);
}
#endif

TEST_CASE("compile panel runs Fast with an injected runner, batches logs and marks stale results") {
    ui_test::app();
    QTemporaryDir dir;
    const auto root = std::filesystem::path(dir.path().toStdU16String());
    std::filesystem::create_directories(root / "game/maps");
    assets::GameProfile game;
    game.gameDirectory = root / "game";
    for (const auto* tool : {"vbsp", "vvis", "vrad", "postcompiler"}) {
        auto file = root / tool;
        std::ofstream(file) << "fake";
        game.toolPaths[tool] = file;
    }
    ui::GameContent content;
    content.reload(&game);
    document::Document doc(editor_test::parsed("world { id 1 classname worldspawn }"));
    compile::test::ScriptedRunner runner;
    compile::test::Script stream;
    for (int i = 0; i < 10000; ++i) stream.lines.push_back("compile line " + std::to_string(i));
    runner.scripts = {stream, {{"post"}}, {{"vis"}}, {{"light"}}};
    ui::CompilePanel panel;
    panel.setGameContent(&content);
    panel.setDocument(&doc);
    panel.setCacheDirectory(root / "cache");
    panel.setProcessRunner(&runner);
    // The copy step still uses real FileOps, entirely inside this temporary directory.
    // Its source is created when the fake lighting step starts.
    struct BspRunner final : compile::ProcessRunner {
        compile::test::ScriptedRunner& scripted;
        explicit BspRunner(compile::test::ScriptedRunner& value) : scripted(value) {}
        Result<std::unique_ptr<compile::ProcessHandle>> start(const std::string& program,
            const std::vector<std::string>& args, const std::filesystem::path& cwd,
            const compile::Environment& env) override {
            std::ofstream(cwd / "untitled.bsp") << "fake bsp";
            return scripted.start(program, args, cwd, env);
        }
    } bspRunner(runner);
    panel.setProcessRunner(&bspRunner);
    auto* run = panel.findChild<QPushButton*>("compileRun");
    auto* table = panel.findChild<QTableWidget*>("compileSteps");
    auto* log = panel.findChild<QPlainTextEdit*>("compileLog");
    auto* header = panel.findChild<QLabel*>("compileHeader");
    REQUIRE(run); REQUIRE(table); REQUIRE(log); REQUIRE(header);
    panel.refresh();
    run->click();
    REQUIRE(table->rowCount() == 6);
    for (int i = 0; i < 5; ++i) CHECK(table->item(i, 1)->text() == "Ok");
    CHECK(table->item(5, 1)->text() == "Skipped");
    CHECK(runner.calls.size() == 4);
    CHECK(runner.calls[0].program == game.toolPath("vbsp").string());
    CHECK(runner.calls[0].cwd == root / "cache/compile/untitled");
    CHECK(std::filesystem::exists(root / "game/maps/untitled.bsp"));
    CHECK(header->text().contains("revision"));
    REQUIRE(until([&] { return log->toPlainText().contains("compile line 9999"); }));
    CHECK(log->document()->blockCount() <= 10000);
    editor_test::addBlock(doc, "entity { classname info_target }");
    panel.refresh();
    CHECK(header->text().contains("stale"));
    panel.setDocument(nullptr);
}

TEST_CASE("compile panel names missing tools before starting any process") {
    ui_test::app();
    QTemporaryDir dir;
    ui::GameContent content;
    assets::GameProfile game;
    game.gameDirectory = std::filesystem::path(dir.path().toStdU16String()) / "game";
    content.reload(&game);
    document::Document doc(vmf::emptyMap());
    compile::test::ScriptedRunner runner;
    ui::CompilePanel panel;
    panel.setDocument(&doc);
    panel.setGameContent(&content);
    panel.setCacheDirectory(dir.path().toStdString());
    panel.setProcessRunner(&runner);
    panel.run();
    CHECK(runner.calls.empty());
    CHECK(panel.findChild<QLabel*>("compileHeader")->text().contains("Missing compile executable $bsp_exe"));
    CHECK(panel.findChild<QTableWidget*>("compileSteps")->item(0, 1)->text() == "Failed");
    panel.setDocument(nullptr);
}

TEST_CASE("compile panel Cancel updates steps and stops the injected process") {
    ui_test::app();
    QTemporaryDir dir;
    const auto root = std::filesystem::path(dir.path().toStdU16String());
    assets::GameProfile game;
    game.gameDirectory = root / "game";
    for (const auto* tool : {"vbsp", "vvis", "vrad"}) {
        std::ofstream(root / tool) << "fake";
        game.toolPaths[tool] = root / tool;
    }
    ui::GameContent content;
    content.reload(&game);
    document::Document doc(vmf::emptyMap());
    compile::test::ScriptedRunner runner;
    runner.scripts.push_back({{}, 0, true});
    ui::CompilePanel panel;
    panel.setDocument(&doc);
    panel.setGameContent(&content);
    panel.setCacheDirectory(root / "cache");
    panel.setProcessRunner(&runner);
    panel.run();
    auto* table = panel.findChild<QTableWidget*>("compileSteps");
    auto* cancel = panel.findChild<QPushButton*>("compileCancel");
    REQUIRE(table); REQUIRE(cancel);
    REQUIRE(runner.handles.size() == 1);
    CHECK(table->item(0, 1)->text() == "Running");
    CHECK(cancel->isEnabled());
    cancel->click();
    CHECK(runner.handles[0]->terminated);
    CHECK(table->item(0, 1)->text() == "Cancelled");
    CHECK_FALSE(cancel->isEnabled());
    CHECK_FALSE(panel.findChild<QPushButton*>("compileRun")->isEnabled());
    panel.run();
    CHECK(runner.calls.size() == 1);
    panel.setDocument(nullptr);
}

TEST_CASE("compile profiles round-trip through the store and the editor") {
    ui_test::app();
    auto profiles = compile::builtinProfiles();
    profiles[1].name = "Mine \"quoted\"";
    profiles[1].cordonOnly = true;
    profiles[1].commands[2].enabled = false;
    profiles[1].commands[5].wait = false;
    CHECK(ui::profilesFromJson(ui::profilesToJson(profiles)) == profiles);
    CHECK(ui::profilesFromJson("not json").empty());
    CHECK(ui::profilesFromJson(R"([1, {"commands": []}, {"name": "ok", "commands": [{"special": 7}]}])").size() == 1);
    CHECK_FALSE(ui::profilesFromJson(R"([{"name": "ok", "commands": [{"special": 7}]}])")[0].commands[0].special);

    // The editor keeps special commands and reorders, adds and removes rows.
    ui::ProfileEditor editor(profiles[1]);
    CHECK(editor.profile() == profiles[1]);
    editor.findChild<QPushButton*>("profileAdd")->click();
    auto edited = editor.profile();
    REQUIRE(edited.commands.size() == profiles[1].commands.size() + 1);
    CHECK(edited.commands.back() == compile::Command{});
    auto* table = editor.findChild<QTableWidget*>("profileCommands");
    table->setCurrentCell(1, 1);
    editor.findChild<QPushButton*>("profileDown")->click();
    edited = editor.profile();
    CHECK(edited.commands[2] == profiles[1].commands[1]);
    CHECK(edited.commands[1] == profiles[1].commands[2]);
    table->setCurrentCell(0, 1);
    editor.findChild<QPushButton*>("profileRemove")->click();
    CHECK(editor.profile().commands.size() == profiles[1].commands.size());
}

TEST_CASE("compile panel restores saved user profiles") {
    ui_test::app();
    QSettings().remove("compile/profiles");
    auto user = compile::builtinProfiles()[0];
    user.name = "Saved";
    user.runGameAfter = false;
    ui::saveUserProfiles({user});
    {
        ui::CompilePanel panel;
        auto* combo = panel.findChild<QComboBox*>("compileProfile");
        REQUIRE(combo);
        CHECK(combo->count() == 4);
        CHECK(combo->itemText(3) == "Saved");
        panel.findChild<QPushButton*>("compileDelete")->click();
        CHECK(combo->count() == 4);
        combo->setCurrentIndex(3);
        panel.findChild<QPushButton*>("compileDelete")->click();
        CHECK(combo->count() == 3);
    }
    CHECK(ui::loadUserProfiles().empty());
    ui::CompilePanel again;
    CHECK(again.findChild<QComboBox*>("compileProfile")->count() == 3);
    QSettings().remove("compile/profiles");
}
