#pragma once
#include "plugin_host/plugin_host.hpp"
#include "ui/plugin_ui.hpp"
#include "viewport/layout.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace apex::ui_test {
// Loads tests/plugin_host/ui_plugin.c: panel plugin.ui.panel, one section per host panel
// (materials, inspector, face_properties), and a dialog opened by clicking "open".
struct UiPluginFixture {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("apex-ui-plugin-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    viewport::AreaRoleRegistry savedRoles = viewport::mutableAreaRoleRegistry();
    std::vector<std::string> logs;
    std::unique_ptr<plugin_host::Host> host;
    std::unique_ptr<ui::PluginUi> ui;
    UiPluginFixture() {
        std::filesystem::create_directories(dir / "ui");
#ifdef _WIN32
        constexpr auto platform = "windows-x86_64";
#else
        constexpr auto platform = "linux-x86_64";
#endif
        std::ofstream(dir / "ui" / "plugin.kv3")
            << "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n"
            << "{ id = \"ui\" name = \"UI\" version = \"1.0.0\" apex_abi = { major = 1 minor = 0 } entry = { \""
            << platform << "\" = \"" << UI_PLUGIN_PATH << "\" } }";
        host = std::make_unique<plugin_host::Host>(dir / "state", dir / "breadcrumb",
                                                   [this](std::string_view line) { logs.emplace_back(line); });
        host->setPanelRegistrar([](std::string id, std::string title) {
            return viewport::mutableAreaRoleRegistry().add({std::move(id), std::move(title)});
        });
        host->discover({dir});
        host->activate();
        ui = std::make_unique<ui::PluginUi>(*host);
    }
    ~UiPluginFixture() {
        ui.reset();
        host.reset();
        viewport::mutableAreaRoleRegistry() = std::move(savedRoles);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
}
