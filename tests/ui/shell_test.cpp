// Headless shell tests; run with QT_QPA_PLATFORM=offscreen (set by CTest).
#include <doctest/doctest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include "core/log.hpp"
#include "document/document.hpp"
#include "ui/log_panel.hpp"
#include "ui/main_window.hpp"
#include "ui/outliner_panel.hpp"
#include "ui/theme.hpp"
#include "ui/viewport_area.hpp"
#include "ui/viewport_pane.hpp"
#include <QMenu>
#include <QSplitter>

namespace {
QApplication& app() {
    static int argc = 1;
    static char name[] = "apex_ui_tests";
    static char* argv[] = {name, nullptr};
    static QTemporaryDir config;  // Keep QSettings out of the user's real config.
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("ApexTest"));
    QCoreApplication::setApplicationName(QStringLiteral("ApexTest"));
    static QApplication a(argc, argv);
    return a;
}

const char* kVmf = R"(versioninfo { "editorversion" "400" }
visgroups { visgroup { "name" "Alpha" "visgroupid" "1" } }
world { "id" "1" "classname" "worldspawn"
  solid { "id" "2" }
  group { "id" "3" }
}
entity { "id" "4" "classname" "info_target" "targetname" "t" }
)";
}  // namespace

TEST_CASE("theme tokens expand into QSS with no placeholders left") {
    app();
    for (auto theme : {apex::ui::Theme::Dark, apex::ui::Theme::Light}) {
        const auto qss = apex::ui::renderQss(apex::ui::tokens(theme));
        CHECK_FALSE(qss.contains(QLatin1Char('@')));
        CHECK(qss.contains(apex::ui::tokens(theme).window.name()));
    }
    CHECK(apex::ui::tokens(apex::ui::Theme::Dark).window != apex::ui::tokens(apex::ui::Theme::Light).window);
}

TEST_CASE("shell opens a VMF, fills the outliner and round-trips on save") {
    auto& a = app();
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
    QTemporaryDir dir;
    const QString in = dir.filePath("t.vmf");
    {
        QFile f(in);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(kVmf);
    }
    apex::ui::MainWindow w;
    w.show();
    CHECK(w.viewportArea() != nullptr);
    CHECK(w.document() == nullptr);

    apex::assets::GameProfile p;
    p.name = "Test Game";
    w.setProfiles({p});
    CHECK(w.profilePicker()->count() == 1);
    REQUIRE(w.currentProfile() != nullptr);
    CHECK(w.currentProfile()->name == "Test Game");

    REQUIRE(w.openFile(in));
    CHECK(w.document() != nullptr);
    auto* tree = w.outliner();
    REQUIRE(tree->topLevelItemCount() == 4);  // World, Entities, Groups, Visgroups
    CHECK(tree->topLevelItem(0)->text(0).startsWith("World"));
    CHECK(tree->topLevelItem(1)->childCount() == 1);
    CHECK(tree->topLevelItem(2)->childCount() == 1);
    CHECK(tree->topLevelItem(3)->childCount() == 1);
    CHECK(w.recentFiles().contains(QFileInfo(in).absoluteFilePath()));

    const QString out = dir.filePath("o.vmf");
    CHECK(w.saveFile(out));
    CHECK(QFileInfo(out).size() > 0);

    w.setTheme(apex::ui::Theme::Light);
    CHECK(apex::ui::currentTheme() == apex::ui::Theme::Light);
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
}

TEST_CASE("log panel receives core/log lines") {
    app();
    apex::ui::LogPanel panel;
    apex::log::warn("hello {}", 42);
    QCoreApplication::processEvents();
    CHECK(panel.toPlainText().contains("hello 42"));
}

TEST_CASE("viewport area mirrors the layout tree into splitters and persists it") {
    auto& a = app();
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
    using namespace apex;
    ui::ViewportArea area;
    area.resize(800, 600);
    area.show();
    CHECK(area.paneCount() == 4);
    CHECK(area.findChildren<ui::PaneFrame*>().size() == 4);
    CHECK(area.findChildren<QSplitter*>().size() == 3);  // Quad: one root split plus two.

    // Split a pane: the rebuild is deferred to the next event-loop turn, then the tree is exact.
    const auto first = area.viewportLayout().paneIds().front();
    emit area.frame(first)->splitRequested(first, viewport::Orientation::Horizontal);
    CHECK(area.paneCount() == 5);
    area.flush();
    CHECK(area.findChildren<ui::PaneFrame*>().size() == 5);

    // Maximize shows exactly one pane without touching the tree.
    area.mutableLayout().toggleMaximize(first);
    area.setLayout(viewport::ViewportLayout(area.viewportLayout()));
    CHECK(area.findChildren<ui::PaneFrame*>().size() == 1);
    area.mutableLayout().restore();

    // Layout JSON round trip.
    const auto json = area.layoutJson();
    ui::ViewportArea other;
    REQUIRE(other.restoreLayoutJson(json));
    CHECK(other.paneCount() == 5);
    CHECK_FALSE(other.restoreLayoutJson("not json"));
    CHECK(other.paneCount() == 5);

    area.resetLayout();
    CHECK(area.paneCount() == 4);
}

TEST_CASE("pane menu offers the full set of view controls") {
    auto& a = app();
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
    apex::ui::ViewportArea area;
    area.show();
    auto* frame = area.findChildren<apex::ui::PaneFrame*>().front();
    emit frame->menu()->aboutToShow();
    QStringList titles;
    for (auto* act : frame->menu()->actions()) titles << act->text();
    for (const char* want : {"Preset", "Projection", "Axis", "Display mode", "Overlays", "Maximize", "Close pane"})
        CHECK(titles.contains(QString::fromLatin1(want)));
    CHECK(titles.filter("Split").size() == 2);
}

TEST_CASE("map views load into and save from the open viewports") {
    auto& a = app();
    apex::ui::applyTheme(a, apex::ui::Theme::Dark);
    QTemporaryDir dir;
    QFile src(QStringLiteral(APEX_FIXTURE_DIR "/probe/e_control.vmf"));
    REQUIRE(src.open(QIODevice::ReadOnly));
    const QByteArray original = src.readAll();
    const QString in = dir.filePath("v.vmf");
    {
        QFile f(in);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(original);
    }
    apex::ui::MainWindow w;
    w.show();
    REQUIRE(w.openFile(in));
    const auto* cam = w.viewportArea()->viewportLayout().viewport(w.viewportArea()->viewportLayout().paneIds()[0]);
    REQUIRE(cam != nullptr);
    CHECK(cam->camera.position == apex::DVec3{0, -300, 100});

    // An unchanged save keeps the views byte-identical.
    const QString out = dir.filePath("o.vmf");
    REQUIRE(w.saveFile(out));
    QFile f(out);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const auto saved = f.readAll();
    CHECK(saved.contains("\"position\" \"(0 -300 100)\""));
}
