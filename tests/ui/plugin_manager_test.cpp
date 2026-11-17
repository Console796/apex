#include "test_app.hpp"
#include "ui/plugin_manager.hpp"
#include "plugin_host/plugin_host.hpp"
#include <doctest/doctest.h>
#include <QCheckBox>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>

TEST_CASE("plugin manager lists errors and persists enabled changes") {
    (void)apex::ui_test::app();
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const auto root = std::filesystem::path(temp.path().toStdString());
    std::filesystem::create_directories(root / "example");
    std::ofstream(root / "example" / "plugin.kv3") <<
        "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n"
        "{ id = \"example\" name = \"Example plugin\" entry = {} }";
    apex::plugin_host::Host host(root / "state", root / "breadcrumb");
    host.discover({root});
    apex::ui::PluginManager manager(host);
    auto* checkbox = manager.findChild<QCheckBox*>();
    REQUIRE(checkbox);
    CHECK(checkbox->text() == QStringLiteral("Example plugin"));
    CHECK(checkbox->isChecked());
    checkbox->setChecked(false);
    CHECK_FALSE(host.plugins().front().enabled);
}
