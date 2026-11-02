// Headless tests for the VRAD view's controller: starting on entry, the stale label after an edit, the
// explicit re-run, cancelling on leaving the mode and the failure text. A scripted runner stands in for
// vbsp/vvis/vrad and a fake loader for the BSP reader, so nothing here needs a GPU or the game.
#include "test_app.hpp"
#include <doctest/doctest.h>
#include "../compile/scripted_runner.hpp"
#include "../editor/fixtures.hpp"
#include "ui/vrad_controller.hpp"

#include <QSettings>
#include <algorithm>
#include <chrono>
#include <fstream>

using namespace apex;
namespace {
namespace fs = std::filesystem;
using compile::test::Script;
using compile::test::ScriptedRunner;

struct Sandbox {
    fs::path path = fs::temp_directory_path() / ("apex-vrad-ui-" + std::to_string(
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
};

struct Harness {
    Sandbox sandbox;
    std::unique_ptr<document::Document> document = std::make_unique<document::Document>(
        editor_test::parsed("world { id 1 classname worldspawn }"));
    ScriptedRunner runner;
    int published = 0, loaded = 0;
    ui::VradController controller;
    Harness()
        : controller(*document, runner,
                     [this](render::VradLightmaps) -> Result<void> { ++published; return {}; },
                     [this](const fs::path&, const document::Document&, std::span<const document::InstanceSolidOrigin>)
                         -> Result<render::VradLightmaps> {
                         ++loaded;
                         render::VradLightmaps maps;
                         maps.stats.faces = 3;
                         return maps;
                     }) {
        apex::ui_test::app();
        QSettings().clear();
        editor_test::addBox(*document, {0, 0, 0}, {64, 64, 64});
    }
    void queue(int runs = 1) { for (int i = 0; i < runs; ++i) runner.scripts.push_back(Script{}); }
    bool update(bool active) { return controller.update(active, sandbox.path / "maps/room.vmf", sandbox.game()); }
};
}

TEST_CASE("the VRAD view compiles on entry and publishes once") {
    Harness h;
    h.queue(3);
    CHECK(h.controller.phase() == ui::VradController::Phase::Idle);
    CHECK(h.controller.status().contains("not run"));
    h.update(false);
    CHECK(h.runner.calls.empty()); // Nothing starts while no pane shows the view.
    CHECK(h.update(true));
    CHECK(h.controller.phase() == ui::VradController::Phase::Ready);
    CHECK(h.runner.calls.size() == 3);
    CHECK(h.published == 1);
    CHECK(h.controller.status().contains("3 faces lit"));
    CHECK_FALSE(h.controller.status().contains("STALE"));
    h.update(true);
    CHECK(h.runner.calls.size() == 3); // Staying in the mode does not recompile.
    CHECK(h.published == 1);
}

TEST_CASE("an edit labels the result stale and Re-run VRAD replaces it") {
    Harness h;
    h.queue(6);
    h.update(true);
    REQUIRE(h.controller.phase() == ui::VradController::Phase::Ready);
    CHECK_FALSE(h.controller.stale());
    editor_test::addBox(*h.document, {128, 0, 0}, {192, 64, 64});
    CHECK(h.controller.stale());
    CHECK(h.controller.status().contains("STALE"));
    CHECK(h.controller.phase() == ui::VradController::Phase::Ready); // The old image stays until a re-run.
    h.update(true);
    CHECK(h.runner.calls.size() == 3);
    h.controller.rerun();
    h.update(true);
    CHECK(h.runner.calls.size() == 6);
    CHECK(h.published == 2);
    CHECK_FALSE(h.controller.stale());
    CHECK_FALSE(h.controller.status().contains("STALE"));
}

TEST_CASE("leaving the mode cancels a run in flight and re-entering starts again") {
    Harness h;
    h.runner.scripts.push_back(Script{.pending = true});
    h.update(true);
    CHECK(h.controller.phase() == ui::VradController::Phase::Running);
    CHECK(h.controller.status().contains("compiling"));
    h.update(false);
    CHECK(h.controller.phase() == ui::VradController::Phase::Idle);
    CHECK(h.published == 0);
    h.queue(3);
    h.update(true);
    CHECK(h.controller.phase() == ui::VradController::Phase::Ready);
    CHECK(h.published == 1);
}

TEST_CASE("a failed tool shows its log tail and no image is published") {
    Harness h;
    h.runner.scripts.push_back(Script{.lines = {"ERROR: leaf 12 has no portal", "second line"}, .code = 1});
    h.update(true);
    CHECK(h.controller.phase() == ui::VradController::Phase::Failed);
    CHECK(h.published == 0);
    CHECK(h.controller.status().startsWith("VRAD failed"));
    CHECK(h.controller.detail().contains("leaf 12"));
    // Re-entering does not retry by itself; the explicit action does.
    h.update(false);
    h.queue(3);
    h.controller.rerun();
    h.update(true);
    CHECK(h.controller.phase() == ui::VradController::Phase::Ready);
}

TEST_CASE("the flags and vis settings reach the tools") {
    Harness h;
    QSettings settings;
    settings.setValue("vrad/flags", "-bounce 2 -noextra");
    settings.setValue("vrad/vis", false);
    h.queue(2);
    h.update(true);
    REQUIRE(h.runner.calls.size() == 2); // vbsp and vrad; vvis is skipped.
    const auto& vrad = h.runner.calls.back().args;
    CHECK(std::ranges::find(vrad, "-bounce") != vrad.end());
    settings.setValue("vrad/displayScale", 2.5);
    h.update(true);
    CHECK(h.controller.scale() == doctest::Approx(2.5f));
}
