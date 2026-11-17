#include "test_app.hpp"
#include "../editor/fixtures.hpp"

#include <doctest/doctest.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "document/document.hpp"
#include "editor/editor_session.hpp"
#include "plugin_host/plugin_host.hpp"
#include "ui/plugin_pump.hpp"

using namespace apex;

// The pump is the host's only clock: with the Qt event loop running, a job finishes on the GUI thread
// and a watched file's save is delivered, with nothing calling drainJobs or pollWatches by hand.
TEST_CASE("the plugin pump delivers job completions and file changes on the GUI thread") {
    (void)ui_test::app();
    const auto dir = std::filesystem::temp_directory_path() /
        ("apex-pump-ui-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir / "async");
#ifdef _WIN32
    constexpr auto platform = "windows-x86_64";
#else
    constexpr auto platform = "linux-x86_64";
#endif
    std::ofstream(dir / "async" / "plugin.kv3")
        << "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n"
        << "{ id = \"async\" name = \"Async\" version = \"1.0.0\" apex_abi = { major = 1 minor = 0 } entry = { \""
        << platform << "\" = \"" << ASYNC_PLUGIN_PATH << "\" } }";
    const auto watched = dir / "async" / "watched.txt";
    std::ofstream(watched) << "one";
    qputenv("APEX_TEST_WATCH", QByteArray::fromStdString(watched.string()));

    document::Document doc{editor_test::parsed("world { id 1 classname worldspawn }")};
    editor::EditorSession session;
    session.attach(doc);
    std::vector<std::string> logs;
    {
        plugin_host::Host host(dir / "state", dir / "breadcrumb", [&](std::string_view line) { logs.emplace_back(line); });
        host.discover({dir});
        host.setDocument(&doc);
        host.setSession(&session);
        host.activate();
        REQUIRE(host.plugins().size() == 1);
        REQUIRE(host.plugins()[0].loaded);
        host.setWatchDebounce(std::chrono::milliseconds(100));
        REQUIRE(session.perform("plugin.async.watch"));
        REQUIRE(session.perform("plugin.async.job"));
        ui::PluginPump pump(host, nullptr, 10);

        const auto has = [&](const char* prefix) {
            return std::ranges::any_of(logs, [&](const std::string& l) { return l.starts_with(prefix); });
        };
        const auto spin = [&](const char* prefix) {
            QElapsedTimer timer;
            timer.start();
            while (!has(prefix) && timer.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            return has(prefix);
        };
        REQUIRE(spin("done:plain"));
        const auto guiThread = std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        const auto done = *std::ranges::find_if(logs, [](const std::string& l) { return l.starts_with("done:plain"); });
        CHECK(done.find(":main=" + guiThread) != std::string::npos);

        std::ofstream(watched, std::ios::trunc) << "two, longer";
        std::filesystem::last_write_time(watched, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
        REQUIRE(spin("changed:"));
        // Keep pumping: the save must not be delivered a second time.
        QElapsedTimer quiet;
        quiet.start();
        while (quiet.elapsed() < 400) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(5); }
        CHECK(std::ranges::count_if(logs, [](const std::string& l) { return l.starts_with("changed:"); }) == 1);
        host.setDocument(nullptr);
        host.setSession(nullptr);
    }
    qunsetenv("APEX_TEST_WATCH");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
