// Headless loader for the plugin_sdk_out_of_tree test: loads the plugins that the nested, SDK-only build
// produced (tests/plugin_sdk/out_of_tree.py) into a real plugin_host::Host and checks they registered.
//
//   apex_plugin_sdk_loader <plugin-root> [--log TEXT] [--perform ACTION] [--section TARGET:ID]
//
// Every plugin folder under <plugin-root> must load without error. --log requires a plugin log line
// containing TEXT; --perform runs the tool action ACTION in an editor session; --section requires the
// ui section ID under TARGET (apex.ui/1).
#include "plugin_host/plugin_host.hpp"
#include "document/vmf.hpp"
#include "document/document.hpp"
#include "editor/editor_session.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    using namespace apex;
    if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin-root> [--log TEXT] [--perform ACTION] [--section TARGET:ID]\n", argv[0]); return 2; }
    const std::filesystem::path root = argv[1];
    std::vector<std::string> logs, wantLog, wantPerform, wantSection;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        (flag == "--log" ? wantLog : flag == "--perform" ? wantPerform : wantSection).emplace_back(argv[i + 1]);
    }
    const auto state = std::filesystem::temp_directory_path() /
        ("apex-sdk-loader-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(state);
    int failures = 0;
    const auto fail = [&](const std::string& what) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; };
    {
        auto file = vmf::parse("world { id 1 classname worldspawn }");
        if (!file) { std::filesystem::remove_all(state); return 2; }
        document::Document document(*file);
        plugin_host::Host host(state / "state", state / "breadcrumb", [&](std::string_view text) { logs.emplace_back(text); });
        host.discover({root});
        host.setDocument(&document);
        host.activate();
        // The session is created after activation: it snapshots the action registry plugins add to.
        editor::EditorSession session;
        session.attach(document);
        host.setSession(&session);
        if (host.plugins().empty()) fail("no plugin found under " + root.string());
        for (const auto& plugin : host.plugins()) {
            std::printf("plugin %s: %s%s\n", plugin.id.c_str(), plugin.loaded ? "loaded" : "NOT loaded",
                        plugin.error.empty() ? "" : (" (" + plugin.error + ")").c_str());
            if (!plugin.loaded || !plugin.error.empty()) fail("plugin " + plugin.id + " did not load");
        }
        for (const auto& text : wantLog) {
            bool seen = false;
            for (const auto& line : logs) seen = seen || line.find(text) != std::string::npos;
            if (!seen) fail("no log line contains '" + text + "'");
        }
        for (const auto& action : wantPerform)
            if (!session.perform(action)) fail("action '" + action + "' is not registered or refused");
        for (const auto& spec : wantSection) {
            const auto colon = spec.find(':');
            bool seen = false;
            for (const auto& section : host.sections(spec.substr(0, colon))) seen = seen || section.id == spec.substr(colon + 1);
            if (!seen) fail("section '" + spec + "' is not registered");
        }
    }
    std::filesystem::remove_all(state);
    std::printf("sdk_loader: %s\n", failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}
