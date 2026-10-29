#include "test_app.hpp"
#include <doctest/doctest.h>
#include "../editor/fixtures.hpp"
#include "../compile/scripted_runner.hpp"
#include "ui/compile_panel.hpp"
#include "ui/main_window.hpp"
#include "ui/viewport_area.hpp"
#include "ui/area_frame.hpp"
#include <QAction>
#include "ui/game_content.hpp"
#include "ui/profile_editor.hpp"
#include "ui/profile_store.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QSignalSpy>
#include "../editor/corpus.hpp"
#include "editor/compile_snapshot.hpp"
#include "compile/map_files.hpp"
#include "document/cordon.hpp"
#include "assets/addon_authoring.hpp"

#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>
#include <algorithm>
#include <fstream>
#include <cstdlib>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#ifdef __linux__
#include <bsppp/bsppp.h>
#endif
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
TEST_CASE("compile_chamber profiles through real QProcessRunner" *
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
    std::optional<editor::CompileCordon> cordon;
    std::string variant = "baseline";
    std::string profileName = "Fast";
    SUBCASE("baseline") {}
    SUBCASE("Final profile") {
        variant = "final";
        profileName = "Final";
    }
    SUBCASE("an entity in the void leaks and leaves a path") {
        variant = "leak";
        editor_test::addBlock(document, "entity { id 990001 classname info_target origin \"8192 8192 8192\" }");
    }
    SUBCASE("cordon snapshot seals a cut through the chamber") {
        variant = "cordon";
        document::addCordon(document, "Cut", AABB{{0, 0, -16}, {320, 350, 256}});
        cordon = editor::CompileCordon{};
    }
    SUBCASE("created instance compiles into an addon target") {
        variant = "instance-addon";
        paths.addonDirectory = root / "addon";
        REQUIRE(assets::createAddon(paths.addonDirectory, {"Compile fixture", true}));
        editor::EditorSession session;
        session.attach(document);
        session.setConnectionContext(source, game.instanceSearchRoot());
        for (auto h : editor_test::nodes(document)) {
            const auto* node = document.get(h);
            const auto* cls = node->block ? node->block->find("classname") : nullptr;
            if (cls && cls->decodedValue().value_or("") == "light") session.selection().add(node->id);
        }
        REQUIRE(session.createInstanceFromSelection(paths.addonDirectory / "mapsrc/instances/light.vmf",
            (paths.addonDirectory / "mapsrc/instances/light.vmf").generic_string()));
    }
    QElapsedTimer snapshotTimer; snapshotTimer.start();
    auto snapshot = editor::writeCompileSnapshot(document, source, root, paths, cordon);
    const double snapshotMs = static_cast<double>(snapshotTimer.nsecsElapsed()) / 1e6;
    REQUIRE_MESSAGE(snapshot, (snapshot ? "" : snapshot.error().what()));
    REQUIRE(outsideInstall(snapshot->paths.workDirectory));
    snapshot->paths.bspDirectory = paths.addonDirectory.empty() ? root / "maps" : paths.addonDirectory / "maps";
    REQUIRE(outsideInstall(snapshot->paths.bspDirectory));
    std::filesystem::create_directories(snapshot->paths.bspDirectory);
    snapshot->paths.variables["bspdir"] = snapshot->paths.bspDirectory.string();
    auto profiles = compile::builtinProfiles();
    const auto chosen = std::ranges::find(profiles, profileName, &compile::CompileProfile::name);
    REQUIRE(chosen != profiles.end());
    auto profile = *chosen;
    profile.runGameAfter = false;
    ui::QProcessRunner runner;
    compile::FilesystemFileOps files;
    compile::CompileJob job(profile, snapshot->paths, runner, files);
    QElapsedTimer compileTimer; compileTimer.start();
    auto started = job.start();
    REQUIRE_MESSAGE(started, (started ? "" : started.error().what()));
    REQUIRE(until([&] { return job.status() != compile::Status::Running; }, 600000));
    const auto stem = source.stem().string();
    if (variant == "leak") {
        // vbsp still succeeds and writes the path; the leak is reported, not hidden behind later steps.
        REQUIRE_FALSE(job.steps().empty());
        CHECK(job.steps().front().status == compile::Status::Ok);
        CHECK(job.status() == compile::Status::Failed); // vvis has no portal file to read
        const auto path = compile::readLeakFile(snapshot->paths.workDirectory, stem);
        REQUIRE(path);
        CHECK(path->size() >= 2);
        const auto diagnostics = compile::collectDiagnostics(job.steps());
        CHECK(std::ranges::any_of(diagnostics, [](const auto& d) { return d.rule == "vbsp.leak"; }));
        MESSAGE("compile gate leak: ", path->size(), " path points; job ", static_cast<int>(job.status()),
            " after ", compileTimer.elapsed(), " ms");
        return;
    }
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
    const auto compileMs = compileTimer.elapsed();
    MESSAGE("compile gate ", variant, ": snapshot ", snapshotMs, " ms; ", profile.name, " ", compileMs, " ms");
    const auto bsp = source.stem().string() + ".bsp";
    REQUIRE(std::filesystem::is_regular_file(snapshot->paths.workDirectory / bsp));
    REQUIRE(std::filesystem::is_regular_file(snapshot->paths.bspDirectory / bsp));
    CHECK(std::filesystem::file_size(snapshot->paths.workDirectory / bsp) ==
          std::filesystem::file_size(snapshot->paths.bspDirectory / bsp));
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        CHECK(entry.path().extension() != ".lin");
    if (variant == "baseline") {
        const auto reference = root / "reference";
        std::filesystem::create_directory(reference);
        std::filesystem::copy_file(source, reference / source.filename());
        const std::vector<std::string> arguments{"-game", game.gameDirectory.string(), "-instancepath",
            game.instanceSearchRoot().string(), (reference / source.filename()).string()};
        QElapsedTimer referenceTimer; referenceTimer.start();
        auto process = runner.start(game.toolPath("vbsp").string(), arguments, reference,
            {{"LD_LIBRARY_PATH", game.toolDirectory().string()}});
        REQUIRE(process);
        std::string referenceLog;
        std::optional<int> code;
        (*process)->setCallbacks([&](std::string line) { referenceLog += line + '\n'; }, [&](int value) { code = value; });
        REQUIRE(until([&] { return code.has_value(); }, 120000));
        REQUIRE_MESSAGE(*code == 0, referenceLog);
        MESSAGE("compile gate baseline: vbsp alone ", referenceTimer.elapsed(), " ms");
        const auto entities = [](const std::filesystem::path& path) {
            bsppp::BSP compiled(path.string());
            REQUIRE(static_cast<bool>(compiled));
            std::vector<std::vector<std::pair<std::string, std::string>>> result;
            for (const auto& entity : compiled.getLumpData<bsppp::BSPLump::ENTITIES>()) {
                std::vector<std::pair<std::string, std::string>> values;
                for (const auto& pair : entity.getKeyValues()) values.emplace_back(pair.getKey(), pair.getValue());
                result.push_back(std::move(values));
            }
            return result;
        };
        CHECK(entities(reference / bsp) == entities(snapshot->paths.workDirectory / bsp));
    }
    if (const auto* output = std::getenv("APEX_COMPILE_GATE_OUT"); output && *output) {
        const auto destination = std::filesystem::path(output) / variant;
        std::filesystem::create_directories(destination);
        std::filesystem::copy(root, destination, std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing);
    }
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

#ifdef _WIN32
TEST_CASE("compile QProcess runner terminates its Windows job descendants") {
    ui_test::app();
    QTemporaryDir directory;
    ui::QProcessRunner runner;
    const std::string program = "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe";
    const std::string script = "$child = Start-Process -FilePath ($env:SystemRoot + '/System32/ping.exe') "
        "-ArgumentList '-n','300','127.0.0.1' -PassThru -WindowStyle Hidden; "
        "Write-Output $child.Id; while ($true) { Start-Sleep -Seconds 1 }";
    auto process = runner.start(program, {"-NoProfile", "-NonInteractive", "-Command", script},
        std::filesystem::path(directory.path().toStdU16String()), {});
    REQUIRE(process);
    DWORD child = 0;
    (*process)->setCallbacks([&](std::string line) {
        bool ok = false;
        const auto pid = QString::fromStdString(line).trimmed().toULong(&ok);
        if (ok) child = pid;
    }, {});
    REQUIRE(until([&] { return child != 0; }));
    HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, child);
    REQUIRE(handle != nullptr);
    (*process)->terminate();
    const bool stopped = WaitForSingleObject(handle, 5000) == WAIT_OBJECT_0;
    if (!stopped) TerminateProcess(handle, 1);
    CloseHandle(handle);
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
    QElapsedTimer streamTimer; streamTimer.start();
    qint64 worstRefreshNs = 0;
    while (!log->toPlainText().contains("compile line 9999") && streamTimer.elapsed() < 5000) {
        QElapsedTimer refreshTimer; refreshTimer.start();
        panel.refresh();
        worstRefreshNs = std::max(worstRefreshNs, refreshTimer.nsecsElapsed());
        QTest::qWait(1);
    }
    REQUIRE(log->toPlainText().contains("compile line 9999"));
    MESSAGE("compile panel drains 10,000 lines in ", streamTimer.elapsed(), " ms; worst 500-line refresh ",
        static_cast<double>(worstRefreshNs) / 1e6, " ms");
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

TEST_CASE("compile panel lists diagnostics, selects the named object and flags stale ids") {
    ui_test::app();
    QTemporaryDir dir;
    const auto root = std::filesystem::path(dir.path().toStdU16String());
    std::filesystem::create_directories(root / "game/maps");
    assets::GameProfile game;
    game.gameDirectory = root / "game";
    for (const auto* tool : {"vbsp", "vvis", "vrad", "postcompiler"}) {
        std::ofstream(root / tool) << "fake";
        game.toolPaths[tool] = root / tool;
    }
    ui::GameContent content;
    content.reload(&game);
    document::Document doc(editor_test::parsed("world { id 1 classname worldspawn }"));
    const auto solid = editor_test::addBox(doc, DVec3{0.0}, DVec3{64.0});
    const auto brushId = doc.get(solid)->block->find("id")->decodedValue().value();
    editor::EditorSession session;
    session.attach(doc);
    compile::test::ScriptedRunner runner;
    compile::test::Script vbsp;
    vbsp.lines = {"Brush " + brushId + ": FloatPlane: bad normal, side 0, texture tile/white_wall_tile003a",
                  "Brush 99999: FloatPlane: bad normal, side 0, texture tile/white_wall_tile003a",
                  "Material not found!: apex/not_a_material"};
    runner.scripts = {vbsp, {{"post"}}, {{"vis"}}, {{"light"}}};
    ui::CompilePanel panel;
    panel.setGameContent(&content);
    panel.setDocument(&doc);
    panel.setEditorSession(&session);
    panel.setCacheDirectory(root / "cache");
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
    auto* table = panel.findChild<QTableWidget*>("compileDiagnostics");
    auto* filter = panel.findChild<QComboBox*>("compileSeverity");
    auto* summary = panel.findChild<QLabel*>("compileSummary");
    REQUIRE(table); REQUIRE(filter); REQUIRE(summary);
    panel.run();
    REQUIRE(until([&] { return table->rowCount() == 3; }));
    CHECK(summary->text().contains("vbsp"));
    QSignalSpy framed(&panel, &ui::CompilePanel::frameRequested);
    panel.activateDiagnostic(0);
    CHECK(framed.count() == 1);
    CHECK(session.selectionBounds().has_value());
    session.selection().clear();
    panel.activateDiagnostic(1);
    CHECK(framed.count() == 1);
    CHECK(table->item(1, 2)->toolTip() == "No longer in the map");
    CHECK_FALSE(session.selectionBounds().has_value());
    filter->setCurrentIndex(2);
    CHECK(table->rowCount() == 2);
    for (int i = 0; i < table->rowCount(); ++i) CHECK(table->item(i, 0)->text() == "Error");
    filter->setCurrentIndex(0);
    CHECK(table->rowCount() == 3);
    panel.setDocument(nullptr);
    panel.setEditorSession(nullptr);
}

TEST_CASE("compile panel shows the leak, then replaces it with portals after a fixed recompile") {
    ui_test::app();
    QTemporaryDir dir;
    const auto root = std::filesystem::path(dir.path().toStdU16String());
    std::filesystem::create_directories(root / "game/maps");
    assets::GameProfile game;
    game.gameDirectory = root / "game";
    for (const auto* tool : {"vbsp", "vvis", "vrad", "postcompiler"}) {
        std::ofstream(root / tool) << "fake";
        game.toolPaths[tool] = root / tool;
    }
    ui::GameContent content;
    content.reload(&game);
    document::Document doc(editor_test::parsed("world { id 1 classname worldspawn }"));
    editor::EditorSession session;
    session.attach(doc);
    compile::test::ScriptedRunner runner;
    ui::CompilePanel panel;
    panel.setGameContent(&content);
    panel.setDocument(&doc);
    panel.setEditorSession(&session);
    panel.setCacheDirectory(root / "cache");
    // vbsp "writes" a leak on the first compile and portals on the second.
    int compiles = 0;
    struct WritingRunner final : compile::ProcessRunner {
        compile::test::ScriptedRunner& scripted;
        int& compiles;
        WritingRunner(compile::test::ScriptedRunner& value, int& count) : scripted(value), compiles(count) {}
        Result<std::unique_ptr<compile::ProcessHandle>> start(const std::string& program,
            const std::vector<std::string>& args, const std::filesystem::path& cwd,
            const compile::Environment& env) override {
            std::ofstream(cwd / "untitled.bsp") << "fake bsp";
            if (program.find("vbsp") != std::string::npos) {
                ++compiles;
                if (compiles == 1) std::ofstream(cwd / "untitled.lin") << "0 0 0\n64 0 0\n64 64 0\n";
                else std::ofstream(cwd / "untitled.prt") << "PRT1\n3\n1\n3 0 2 (0 0 0 ) (1 0 0 ) (1 1 0 ) \n";
            }
            return scripted.start(program, args, cwd, env);
        }
    } writing(runner, compiles);
    panel.setProcessRunner(&writing);
    runner.scripts = {{{"bsp"}}, {{"post"}}, {{"vis"}}, {{"light"}}};
    auto* goTo = panel.findChild<QPushButton*>("compileGoToLeak");
    auto* clusters = panel.findChild<QComboBox*>("compileCluster");
    REQUIRE(goTo); REQUIRE(clusters);
    std::vector<DVec3> focused;
    QObject::connect(&panel, &ui::CompilePanel::focusRequested, &panel, [&](DVec3 point) { focused.push_back(point); });
    CHECK_FALSE(goTo->isEnabled());

    panel.run();
    REQUIRE(until([&] { return session.compileOverlay().has_value(); }));
    REQUIRE(session.compileOverlay()->leak.size() == 3);
    CHECK(session.compileOverlay()->portals.portals.empty());
    CHECK(session.compileOverlay()->revision == doc.historyState());
    CHECK(goTo->isEnabled());
    goTo->click();
    REQUIRE(focused.size() == 1);
    CHECK(focused[0] == DVec3(0, 0, 0));
    // Hiding the leak keeps the data but not the drawing.
    panel.findChild<QCheckBox*>("compileShowLeak")->setChecked(false);
    CHECK_FALSE(session.compileOverlay()->showLeak);

    editor_test::addBlock(doc, "entity { classname info_target }");
    CHECK(session.compileOverlay()->revision != doc.historyState()); // drawn grey from here on

    runner.scripts = {{{"bsp"}}, {{"post"}}, {{"vis"}}, {{"light"}}};
    runner.calls.clear();
    panel.run();
    REQUIRE(until([&] { return session.compileOverlay() && session.compileOverlay()->leak.empty(); }));
    CHECK(session.compileOverlay()->portals.portals.size() == 1);
    CHECK(session.compileOverlay()->revision == doc.historyState());
    CHECK_FALSE(goTo->isEnabled());
    CHECK(clusters->count() == 4); // all + clusters 0..2
    clusters->setCurrentIndex(2);
    CHECK(session.compileOverlay()->cluster == 1);
    panel.setDocument(nullptr);
    panel.setEditorSession(nullptr);
}

TEST_CASE("compile profiles round-trip through the store and the editor") {
    ui_test::app();
    auto profiles = compile::builtinProfiles();
    profiles[1].name = "Mine \"quoted\"";
    profiles[1].addonDirectory = "/tmp/addon with spaces";
    profiles[1].campaignId = "main";
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

TEST_CASE("cordon editor adds, edits, deactivates and removes cordons as undoable steps") {
    document::Document doc(editor_test::parsed("world { id 1 classname worldspawn }"));
    ui::CompilePanel panel;
    panel.setDocument(&doc);
    auto* list = panel.findChild<QComboBox*>("cordonList");
    auto* add = panel.findChild<QPushButton*>("cordonAdd");
    auto* active = panel.findChild<QCheckBox*>("cordonActive");
    auto* maxX = panel.findChild<QDoubleSpinBox*>("cordonMaxX");
    auto* remove = panel.findChild<QPushButton*>("cordonRemove");
    REQUIRE(list); REQUIRE(add); REQUIRE(active); REQUIRE(maxX); REQUIRE(remove);
    CHECK(list->count() == 0);
    CHECK_FALSE(remove->isEnabled());
    add->click();
    REQUIRE(list->count() == 1);
    CHECK(maxX->value() == 256.0);
    maxX->setValue(512.0);
    CHECK(document::readCordons(doc).boxes[0].box.max.x == 512.0);
    active->setChecked(false);
    CHECK(document::readCordons(doc).compileBoxes().empty());
    CHECK(list->itemText(0).contains("off"));
    remove->click();
    CHECK(list->count() == 0);
    CHECK(doc.undo());
    CHECK(list->count() == 1);
}

TEST_CASE("compile panel lists problems, gates a compile on errors and reports problems before") {
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
    document::Document doc(editor_test::parsed(
        "world { id 1 classname worldspawn }\n"
        "entity { id 20 classname func_detail }\n"));
    compile::test::ScriptedRunner runner;
    runner.scripts.push_back({{}, 0, true});
    ui::CompilePanel panel;
    panel.setDocument(&doc);
    panel.setGameContent(&content);
    panel.setCacheDirectory(root / "cache");
    panel.setProcessRunner(&runner);
    auto* table = panel.findChild<QTableWidget*>("problemsTable");
    auto* filter = panel.findChild<QComboBox*>("problemsFilter");
    auto* summary = panel.findChild<QLabel*>("problemsSummary");
    REQUIRE(table); REQUIRE(filter); REQUIRE(summary);

    panel.findChild<QPushButton*>("problemsCheck")->click();
    CHECK(summary->text().contains("1 errors, 1 warnings"));
    CHECK(table->rowCount() == 2); // the empty brush entity, then the missing info_player_start
    filter->setCurrentIndex(2);
    CHECK(table->rowCount() == 1);
    CHECK(table->item(0, 1)->text().contains("no brushes"));
    filter->setCurrentIndex(0);

    int asked = 0;
    bool allow = false;
    panel.setProblemGate([&](int errors, int warnings) { ++asked; CHECK(errors == 1); CHECK(warnings == 1); return allow; });
    panel.run();
    CHECK(asked == 1);
    CHECK(runner.handles.empty()); // the gate declined, so nothing started

    allow = true;
    panel.run();
    CHECK(asked == 2);
    REQUIRE(runner.handles.size() == 1);
    panel.refresh();
    CHECK(panel.findChild<QLabel*>("compileSummary")->text().contains("Problems before: 1 errors, 1 warnings"));

    // Fixing the planted problems clears the list and the gate no longer asks.
    panel.cancel();
    document::Document clean(editor_test::parsed(
        "world { id 1 classname worldspawn }\nentity { id 2 classname info_player_start origin \"0 0 0\" }\n"));
    panel.setDocument(&clean);
    CHECK(panel.checkProblems().empty());
    CHECK(summary->text().contains("No problems"));
    panel.setDocument(nullptr);
}

TEST_CASE("compile panel runs the game detached, tails its console log and reloads the map") {
    ui_test::app();
    QTemporaryDir dir;
    const auto root = std::filesystem::path(dir.path().toStdU16String());
    assets::GameProfile game;
    game.gameDirectory = root / "Portal 2 CE" / "p2ce";
    std::filesystem::create_directories(game.gameDirectory / "maps");
    ui::GameContent content;
    content.reload(&game);
    document::Document doc(vmf::emptyMap());
    ui::CompilePanel panel;
    compile::test::ScriptedRunner runner;
    panel.setGameContent(&content);
    panel.setDocument(&doc);
    panel.setSourcePath(root / "my map.vmf");
    panel.setProcessRunner(&runner);
    auto* header = panel.findChild<QLabel*>("compileHeader");
    auto* log = panel.findChild<QPlainTextEdit*>("gameLog");
    auto* reload = panel.findChild<QPushButton*>("gameReloadMap");
    auto* runGame = panel.findChild<QPushButton*>("compileRunGame");
    REQUIRE(header); REQUIRE(log); REQUIRE(reload); REQUIRE(runGame);

    SUBCASE("no compiled map") {
        runGame->click();
        CHECK(runner.detached.empty());
        CHECK(header->text().contains("no compiled map", Qt::CaseInsensitive));
    }
    SUBCASE("compiled map") {
        std::ofstream(game.gameDirectory / "maps/my map.bsp") << "bsp";
        std::ofstream(game.gameDirectory / "console.log") << "old line\n";
        runGame->click();
        REQUIRE(runner.detached.size() == 1);
        CHECK(runner.detached[0].program == game.toolPath("game").string());
        CHECK(runner.detached[0].args == std::vector<std::string>{"-dev", "-console", "-allowdebug", "-game",
            game.gameDirectory.string(), "+map", "my map", "-condebug"});
        CHECK(header->text().isEmpty());
        // Only what the game writes after the launch is shown.
        std::ofstream(game.gameDirectory / "console.log", std::ios::app) << "Map: my map\nloaded\n";
        panel.pollGameLog();
        CHECK(panel.gameLogText() == "Map: my map\nloaded");
        // Reload map re-runs the launch and starts the log view again from the end of the file.
        reload->click();
        CHECK(runner.detached.size() == 2);
        CHECK(panel.gameLogText().isEmpty());
        std::ofstream(game.gameDirectory / "console.log", std::ios::trunc) << "fresh\n";
        panel.pollGameLog();
        CHECK(panel.gameLogText() == "fresh");
    }
    SUBCASE("start failure is reported") {
        std::ofstream(game.gameDirectory / "maps/my map.bsp") << "bsp";
        runner.failDetached = true;
        runGame->click();
        CHECK(header->text().contains("Cannot start"));
    }
    panel.setDocument(nullptr);
}

TEST_CASE("compilation role preserves a running job across layouts and floating windows") {
    ui_test::app();
    QTemporaryDir directory;
    const auto root = std::filesystem::path(directory.path().toStdU16String());
    assets::GameProfile game;
    game.gameDirectory = root / "game";
    std::filesystem::create_directories(game.gameDirectory);
    std::ofstream(game.gameDirectory / "gameinfo.txt") << "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }";
    for (const auto* tool : {"vbsp", "vvis", "vrad", "postcompiler"}) {
        std::ofstream(root / tool) << "fake";
        game.toolPaths[tool] = root / tool;
    }
    ui::GameContent content;
    content.reload(&game);
    document::Document document(editor_test::parsed("world { id 1 classname worldspawn }"));
    compile::test::ScriptedRunner runner;
    runner.scripts = {{{}, 0, true}};
    ui::ViewportArea area;
    area.setGameContent(&content);
    area.setDocument(&document);
    viewport::ViewportLayout dedicated;
    const auto pane = dedicated.paneIds().front();
    REQUIRE(dedicated.setRole(pane, viewport::AreaRole::Compile));
    auto decoded = viewport::ViewportLayout::fromJson(dedicated.toJson());
    REQUIRE(decoded);
    CHECK(decoded->role(pane) == viewport::AreaRole::Compile);
    area.setLayout(dedicated);
    area.resize(640, 480);
    area.show();
    ui_test::app().processEvents();
    auto* panel = area.compilePanel();
    panel->setCacheDirectory(root / "cache");
    panel->setProcessRunner(&runner);
    auto* profile = panel->findChild<QComboBox*>("compileProfile");
    auto* log = panel->findChild<QPlainTextEdit*>("compileLog");
    REQUIRE(profile); REQUIRE(log);
    profile->setCurrentIndex(0);
    panel->run();
    REQUIRE(runner.handles.size() == 1);
    CHECK_FALSE(runner.handles.front()->terminated);
    log->setPlainText("retained compilation output");
    CHECK(area.minimumSizeHint().width() <= 640);
    CHECK(area.minimumSizeHint().height() <= 480);
    area.setLayout(viewport::ViewportLayout{});
    CHECK(area.compilePanel() == panel);
    CHECK_FALSE(runner.handles.front()->terminated);
    auto* floating = qobject_cast<ui::CompilePanelView*>(area.showRole(viewport::AreaRole::Compile, true));
    REQUIRE(floating);
    CHECK(floating->compilePanel() == panel);
    CHECK(floating->isAncestorOf(panel));
    CHECK(log->toPlainText() == "retained compilation output");
    area.setLayout(dedicated);
    auto* embedded = qobject_cast<ui::CompilePanelView*>(area.showRole(viewport::AreaRole::Compile, false));
    REQUIRE(embedded);
    CHECK(embedded->isAncestorOf(panel));
    CHECK_FALSE(floating->isAncestorOf(panel));
    CHECK_FALSE(runner.handles.front()->terminated);
    CHECK(log->toPlainText() == "retained compilation output");
    panel->cancel();
    CHECK(runner.handles.front()->terminated);
    panel->setDocument(nullptr);
}

TEST_CASE("compilation menu respects workspace and floating window preferences") {
    ui_test::app();
    const auto previous = QSettings().value("ui/compilationWindow");
    ui::MainWindow window;
    auto* area = window.viewportArea();
    viewport::ViewportLayout layout;
    const auto pane = layout.paneIds().front();
    REQUIRE(layout.setRole(pane, viewport::AreaRole::Compile));
    area->setLayout(layout);
    window.show();
    ui::MainWindow::setCompilationInWindow(false);
    auto* show = window.findChild<QAction*>("compile.show");
    REQUIRE(show);
    show->trigger();
    CHECK_FALSE(area->floating(viewport::AreaRole::Compile));
    CHECK(area->panel(pane)->isAncestorOf(area->compilePanel()));
    ui::MainWindow::setCompilationInWindow(true);
    show->trigger();
    REQUIRE(area->floating(viewport::AreaRole::Compile));
    CHECK(area->floating(viewport::AreaRole::Compile)->isVisible());
    CHECK(area->floating(viewport::AreaRole::Compile)->isAncestorOf(area->compilePanel()));
    CHECK_FALSE(window.findChild<QWidget*>("compileDock"));
    QSettings().setValue("ui/compilationWindow", previous);
}
