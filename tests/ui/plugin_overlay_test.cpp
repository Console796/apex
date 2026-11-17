#include "test_app.hpp"
#include "../editor/fixtures.hpp"

#include <doctest/doctest.h>
#include <QCheckBox>
#include <QToolButton>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "core/entity_class.hpp"
#include "document/document.hpp"
#include "editor/editor_session.hpp"
#include "editor/map_problems.hpp"
#include "plugin_host/plugin_host.hpp"
#include "ui/plugin_ui.hpp"
#include "ui/theme.hpp"
#include "ui/viewport_area.hpp"
#include "ui/viewport_pane.hpp"
#include "viewport/layout.hpp"

using namespace apex;
namespace {
// Loads tests/plugin_host/overlay_plugin.cpp as plugin "wrap": overlays plugin.wrap.main (one segment, one point,
// one label, a tint and a ghost cube) and plugin.wrap.other (empty), and the helper "wrapwidget".
struct OverlayFixture {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("apex-overlay-ui-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::vector<std::string> logs;
    document::Document doc{editor_test::parsed(
        "world { id 1 classname worldspawn } entity { id 7 classname wrapent origin \"32 0 0\" }")};
    editor::EditorSession session;
    editor::ProblemRegistry problems; // the process-wide registry may already be frozen by earlier tests
    std::unique_ptr<plugin_host::Host> host;
    std::unique_ptr<ui::PluginUi> ui;
    OverlayFixture() {
        std::filesystem::create_directories(dir / "wrap");
#ifdef _WIN32
        constexpr auto platform = "windows-x86_64";
#else
        constexpr auto platform = "linux-x86_64";
#endif
        std::ofstream(dir / "wrap" / "plugin.kv3")
            << "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n"
            << "{ id = \"wrap\" name = \"Wrap\" version = \"1.0.0\" apex_abi = { major = 1 minor = 0 } entry = { \""
            << platform << "\" = \"" << OVERLAY_PLUGIN_PATH << "\" } }";
        session.attach(doc);
        assets::EntityClassTable classes;
        assets::EntityClass cls; cls.name = "wrapent"; cls.type = "PointClass";
        cls.helpers = {{"wrapwidget", ""}};
        classes.classes["wrapent"] = std::move(cls);
        session.setEntityClasses(std::move(classes));
        host = std::make_unique<plugin_host::Host>(dir / "state", dir / "breadcrumb",
                                                   [this](std::string_view line) { logs.emplace_back(line); });
        host->useProblemRegistry(problems);
        host->discover({dir});
        host->setDocument(&doc);
        host->setSession(&session);
        host->activate();
        ui = std::make_unique<ui::PluginUi>(*host);
    }
    ~OverlayFixture() {
        ui.reset();
        host.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
}

TEST_CASE("pane plugin overlays draw in the plugin's lists, scale with dpr and honour the per-pane toggle") {
    (void)ui_test::app();
    OverlayFixture f;
    REQUIRE(f.host->plugins().size() == 1);
    REQUIRE(f.host->plugins()[0].loaded);
    auto view = viewport::makePreset(viewport::Preset::Camera);

    render::DrawList base;
    ui::appendPluginOverlays(base, *f.host, view.overlays, 1.0);
    // Overlay: 1 line + 12 ghost edges, 1 point, 12 ghost triangles, 1 label. Helpers add 1 segment + 1 label.
    CHECK(base.worldPoints.size() == 1);
    CHECK(base.worldTriangles.size() == 12);
    CHECK(base.worldSegments.size() == 13 + 1);
    CHECK(base.labels.size() == 1 + 1);
    CHECK(base.labelScale == 1.0f);

    // Widths and sizes follow the device pixel ratio.
    render::DrawList scaled;
    ui::appendPluginOverlays(scaled, *f.host, view.overlays, 2.0);
    REQUIRE(scaled.worldSegments.size() == base.worldSegments.size());
    for (std::size_t i = 0; i < base.worldSegments.size(); ++i) CHECK(scaled.worldSegments[i].widthPixels == doctest::Approx(base.worldSegments[i].widthPixels * 2.0f));
    CHECK(scaled.worldPoints[0].sizePixels == doctest::Approx(base.worldPoints[0].sizePixels * 2.0f));
    CHECK(scaled.labelScale == 2.0f);

    // Helpers show only while entity helpers are on.
    view.overlays.entityHelpers = false;
    render::DrawList noHelpers;
    ui::appendPluginOverlays(noHelpers, *f.host, view.overlays, 1.0);
    CHECK(noHelpers.worldSegments.size() == 13);
    CHECK(noHelpers.labels.size() == 1);
    view.overlays.entityHelpers = true;

    // Tints: the overlay's side tint appears, and turning that overlay off drops it.
    auto tints = ui::pluginSideTints(*f.host, view.overlays);
    REQUIRE(tints.size() == 1);
    CHECK(tints.at(5).a == doctest::Approx(0.5f));

    // Toggling the overlay off for this pane drops all of its lists in every display mode but keeps helpers.
    view.overlays.pluginOff.insert("plugin.wrap.main");
    render::DrawList off;
    ui::appendPluginOverlays(off, *f.host, view.overlays, 1.0);
    CHECK(off.worldPoints.empty());
    CHECK(off.worldTriangles.empty());
    CHECK(off.worldSegments.size() == 1);
    CHECK(off.labels.size() == 1);
    CHECK(ui::pluginSideTints(*f.host, view.overlays).empty());
    // Another pane's viewport is unaffected.
    const auto other = viewport::makePreset(viewport::Preset::Top);
    CHECK(ui::pluginSideTints(*f.host, other.overlays).size() == 1);
}

TEST_CASE("overlay panel lists plugin overlays and toggling one edits only that pane") {
    auto& a = ui_test::app();
    ui::applyTheme(a, ui::Theme::Dark);
    OverlayFixture f;
    REQUIRE(f.host->overlays().size() == 2);
    ui::ViewportArea area;
    area.setPluginUi(f.ui.get());
    area.show();
    auto* frame = area.frame(area.viewportLayout().active());
    REQUIRE(frame);
    const auto viewport = [&] { return area.viewportLayout().viewport(frame->paneId()); };
    REQUIRE(viewport());
    auto* button = frame->findChild<QToolButton*>("PaneOverlaysButton");
    REQUIRE(button);
    button->click();
    auto* panel = frame->overlayPanel();
    REQUIRE(panel);
    auto* main = panel->pluginBox("plugin.wrap.main");
    auto* second = panel->pluginBox("plugin.wrap.other");
    REQUIRE(main);
    REQUIRE(second);
    CHECK(main->text() == "Wrap overlay");
    CHECK(panel->pluginBox("plugin.nope") == nullptr);
    CHECK(main->isChecked());
    CHECK(viewport()->overlays.pluginOff.empty());
    main->click();
    CHECK(viewport()->overlays.pluginOff.contains("plugin.wrap.main"));
    CHECK_FALSE(viewport()->overlays.pluginOff.contains("plugin.wrap.other"));
    CHECK(panel->isVisible());
    // Reopening shows the stored state.
    panel->hide();
    CHECK(area.showOverlays());
    CHECK_FALSE(panel->pluginBox("plugin.wrap.main")->isChecked());
    panel->pluginBox("plugin.wrap.main")->click();
    CHECK(viewport()->overlays.pluginOff.empty());
}
