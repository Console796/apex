#include "test_app.hpp"
#include "plugin_ui_fixture.hpp"
#include "../editor/fixtures.hpp"

#include <doctest/doctest.h>
#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTest>

#include "document/document.hpp"
#include "editor/editor_session.hpp"
#include "ui/inspector_panel.hpp"
#include "ui/viewport_area.hpp"
#include "viewport/layout.hpp"

using namespace apex;
namespace {
QString summaryOf(QWidget& root, const QString& section) {
    auto* box = root.findChild<QGroupBox*>(section);
    if (!box) return {};
    auto* label = box->findChild<QLabel*>("summary");
    return label ? label->text() : QString{};
}
}

TEST_CASE("plugin panel keeps its widgets and focus when the plugin re-describes it") {
    (void)ui_test::app();
    ui_test::UiPluginFixture f;
    REQUIRE(f.ui->hasPanel("plugin.ui.panel"));
    std::unique_ptr<QWidget> panel(f.ui->createPanel("plugin.ui.panel"));
    REQUIRE(panel);
    panel->resize(300, 300);
    panel->show();
    QCoreApplication::processEvents();
    auto* field = panel->findChild<QLineEdit*>("field");
    REQUIRE(field);
    CHECK(field->text() == "value");
    field->setFocus();
    QCoreApplication::processEvents();
    REQUIRE(field->hasFocus());
    QSignalSpy refreshed(f.ui.get(), &ui::PluginUi::formRefreshed);
    QTest::keyClicks(field, "x");  // The plugin logs "changed" and asks for a refresh from inside its callback.
    CHECK(f.logs == std::vector<std::string>{"changed"});
    CHECK(refreshed.count() == 0);  // Queued, not re-entrant.
    f.ui->flush();
    REQUIRE(refreshed.count() == 1);
    CHECK(refreshed.front().front().toString() == "plugin.ui.panel");
    CHECK(panel->findChild<QLineEdit*>("field") == field);
    CHECK(field->hasFocus());
    CHECK(field->text() == "value");  // The plugin's description wins.
    // Several requests before the loop turns are coalesced into one re-describe.
    f.host->panelChanged("plugin.ui.panel", "field", apex_ui_value{sizeof(apex_ui_value), 0, {}, 0});
    f.host->panelChanged("plugin.ui.panel", "field", apex_ui_value{sizeof(apex_ui_value), 0, {}, 0});
    f.ui->flush();
    CHECK(refreshed.count() == 2);
}

TEST_CASE("plugin dialogs are built from the form description and report accept or cancel") {
    (void)ui_test::app();
    ui_test::UiPluginFixture f;
    QString title; bool hadField = false;
    bool accept = true;
    f.ui->setDialogHook([&](QDialog& dialog) {
        title = dialog.windowTitle();
        if (auto* edit = dialog.findChild<QLineEdit*>("d")) { hadField = true; QTest::keyClicks(edit, "z"); }
        return accept;
    });
    f.host->panelClicked("plugin.ui.panel", "open");
    CHECK(title == "Options");
    CHECK(hadField);
    CHECK(f.logs == std::vector<std::string>{"dialog changed", "dialog accepted"});
    f.logs.clear();
    accept = false;
    f.host->panelClicked("plugin.ui.panel", "open");
    CHECK(f.logs == std::vector<std::string>{"dialog changed", "dialog cancelled"});
}

TEST_CASE("plugin sections get the context their host panel names") {
    (void)ui_test::app();
    ui_test::UiPluginFixture f;
    {
        ui::PluginSections sections("materials");
        sections.setPluginUi(f.ui.get());
        sections.setContext({"materials", {"dev/a", "dev/b"}, {}});
        CHECK(summaryOf(sections, "plugin.ui.section") == "materials names=2 ids=0 first=dev/a");
        sections.setContext({"materials", {}, {}});
        CHECK(summaryOf(sections, "plugin.ui.section") == "materials names=0 ids=0 first=");
        CHECK_FALSE(sections.findChild<QGroupBox*>("plugin.ui.inspector"));  // Another target's section.
    }
    {
        ui::PluginSections sections("face_properties");
        sections.setPluginUi(f.ui.get());
        sections.setContext({"sides", {}, {7, 9, 11}});
        CHECK(summaryOf(sections, "plugin.ui.faces") == "sides names=0 ids=3 first=");
    }
    {
        ui::PluginSections sections("nowhere");
        sections.setPluginUi(f.ui.get());
        CHECK(sections.findChildren<QGroupBox*>().isEmpty());
    }
}

TEST_CASE("inspector plugin sections receive the selected entities") {
    (void)ui_test::app();
    ui_test::UiPluginFixture f;
    document::Document doc(editor_test::parsed(R"(entity { id 1 classname info_target } entity { id 2 classname info_target })"));
    editor::EditorSession session;
    session.attach(doc);
    ui::InspectorPanel panel;
    panel.setDocument(&doc);
    panel.setEditorSession(&session);
    panel.setPluginUi(f.ui.get());
    panel.resize(320, 600);
    panel.show();
    session.selectNode(doc.roots()[0]);
    QCoreApplication::processEvents();
    CHECK(summaryOf(panel, "plugin.ui.inspector") == "entities names=0 ids=1 first=");
    session.selectNode(doc.roots()[0]);
    session.selection().add(doc.roots()[1]);
    panel.refresh();
    CHECK(summaryOf(panel, "plugin.ui.inspector") == "entities names=0 ids=2 first=");
    panel.setPluginUi(nullptr);
    CHECK_FALSE(panel.findChild<QGroupBox*>("plugin.ui.inspector"));
}

TEST_CASE("a saved layout keeps a plugin panel as a placeholder until the plugin returns") {
    (void)ui_test::app();
    ui_test::UiPluginFixture f;
    QByteArray saved;
    {
        ui::ViewportArea area;
        area.setPluginUi(f.ui.get());
        area.resize(800, 600);
        area.show();
        auto layout = viewport::ViewportLayout(area.viewportLayout());
        const auto pane = layout.paneIds().front();
        REQUIRE(layout.setPluginPanel(pane, "plugin.ui.panel"));
        area.setLayout(std::move(layout));
        area.flush();
        CHECK(area.findChild<QWidget*>("PluginPanel"));
        CHECK_FALSE(area.findChild<QLabel*>("MissingPluginPanel"));
        saved = area.layoutJson();
    }
    CHECK(saved.contains("plugin.ui.panel"));
    // The plugin is gone: the panel is shown as a placeholder and the layout still names it.
    ui::ViewportArea absent;
    absent.resize(800, 600);
    absent.show();
    REQUIRE(absent.restoreLayoutJson(saved));
    absent.flush();
    auto* placeholder = absent.findChild<QLabel*>("MissingPluginPanel");
    REQUIRE(placeholder);
    CHECK(placeholder->property("panelId").toString() == "plugin.ui.panel");
    CHECK(placeholder->text().contains("plugin.ui.panel"));
    CHECK_FALSE(absent.findChild<QWidget*>("PluginPanel"));
    CHECK(absent.layoutJson().contains("plugin.ui.panel"));
    // The plugin returns: the placeholder becomes the real panel in the same pane.
    absent.setPluginUi(f.ui.get());
    absent.flush();
    CHECK_FALSE(absent.findChild<QLabel*>("MissingPluginPanel"));
    auto* real = absent.findChild<QWidget*>("PluginPanel");
    REQUIRE(real);
    CHECK(real->property("panelId").toString() == "plugin.ui.panel");
    CHECK(real->findChild<QLineEdit*>("field"));
    CHECK(absent.layoutJson() == saved);
}
