#include <doctest/doctest.h>

#include "editor/autosave.hpp"
#include "document/document.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace apex;
namespace {
namespace fs = std::filesystem;
struct StateDir {
    fs::path path = fs::temp_directory_path() / ("apex-autosave-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    StateDir() { fs::create_directories(path); }
    ~StateDir() { std::error_code ignored; fs::remove_all(path, ignored); }
};
std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
std::vector<fs::path> vmfs(const fs::path& root) {
    std::vector<fs::path> result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.path().extension() == ".vmf") result.push_back(entry.path());
    return result;
}
}

TEST_CASE("autosave snapshots exact bytes only on history changes and removes clean locks") {
    StateDir dir;
    auto parsed = vmf::parse("world { id 1 classname worldspawn }");
    REQUIRE(parsed);
    document::Document doc(*parsed);
    fs::path lock;
    {
        editor::Autosave save("example.vmf", dir.path);
        lock = save.lockPath();
        CHECK(fs::exists(lock));
        REQUIRE(save.snapshot(doc).value());
        REQUIRE(save.wait());
        REQUIRE(vmfs(dir.path).size() == 1);
        CHECK(read(vmfs(dir.path).front()) == doc.write().value());
        CHECK_FALSE(save.snapshot(doc).value());
        CHECK(editor::findRecoveries(dir.path)->empty());
        std::set<std::string> expected;
        for (int i = 0; i < 13; ++i) {
            auto edit = doc.begin("change");
            edit.setKeyValue(*doc.handle(doc.roots().front()), "message", std::to_string(i));
            edit.commit(false);
            if (i >= 3) expected.insert(doc.write().value());
            REQUIRE(save.snapshot(doc).value());
            REQUIRE(save.wait());
        }
        CHECK(vmfs(dir.path).size() == 10);
        std::set<std::string> actual;
        for (const auto& path : vmfs(dir.path)) actual.insert(read(path));
        CHECK(actual == expected);
        REQUIRE(doc.undo());
        REQUIRE(save.snapshot(doc).value());
        REQUIRE(save.wait());
    }
    CHECK_FALSE(fs::exists(lock));
    CHECK(editor::findRecoveries(dir.path)->empty());
}

#ifndef _WIN32
TEST_CASE("SIGKILL leaves a recoverable snapshot and stale session lock") {
    StateDir dir;
    int ready[2];
    REQUIRE(pipe(ready) == 0);
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        close(ready[0]);
        auto parsed = vmf::parse("world { id 1 classname worldspawn }");
        document::Document doc(*parsed);
        editor::Autosave save("crashed.vmf", dir.path);
        if (!save.snapshot(doc) || !save.wait()) _exit(2);
        const char byte = 'x';
        (void)write(ready[1], &byte, 1);
        for (;;) pause();
    }
    close(ready[1]);
    char byte = 0;
    const auto received = ::read(ready[0], &byte, 1);
    close(ready[0]);
    CHECK(received == 1);
    CHECK(editor::findRecoveries(dir.path)->empty());
    CHECK(kill(child, SIGKILL) == 0);
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);
    const auto entries = editor::findRecoveries(dir.path);
    REQUIRE(entries);
    REQUIRE(entries->size() == 1);
    const auto& entry = entries->front();
    CHECK(entry.source == fs::absolute("crashed.vmf"));
    CHECK(fs::exists(entry.snapshot));
    auto parsed = vmf::parse("world { id 1 classname worldspawn }");
    document::Document doc(*parsed);
    CHECK(read(entry.snapshot) == doc.write().value());
    REQUIRE(editor::discardRecovery(entry));
    CHECK(editor::findRecoveries(dir.path)->empty());
    CHECK(vmfs(dir.path).empty());
}
#endif

TEST_CASE("autosave D16 measures main-thread serialization on the largest corpus map") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping D16 serialization measurement");
        return;
    }
    const auto root = fs::path(install) / "sdk_content/maps";
    REQUIRE(fs::is_directory(root));
    fs::path largest;
    std::uintmax_t size = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf" && entry.file_size() > size) {
            largest = entry.path();
            size = entry.file_size();
        }
    }
    REQUIRE_FALSE(largest.empty());
    const auto bytes = read(largest);
    auto parsed = vmf::parse(bytes);
    if (!parsed) parsed = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
    REQUIRE(parsed);
    document::Document doc(*parsed);
    const auto start = std::chrono::steady_clock::now();
    const auto serialized = doc.write();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(serialized);
    CHECK(*serialized == bytes);
    MESSAGE("D16 Document::write(): ", largest.string(), ", ", size, " bytes, ", ms, " ms");
}

TEST_CASE("autosave retention keeps newest ten exact snapshots across session restarts") {
    StateDir dir;
    auto parsed = vmf::parse("world { id 1 classname worldspawn }");
    REQUIRE(parsed);
    document::Document doc(*parsed);
    std::vector<std::string> expected;
    fs::path directory;
    for (int launch = 0; launch < 3; ++launch) {
        editor::Autosave save("restarted.vmf", dir.path);
        if (launch) CHECK(save.lockPath().parent_path() == directory);
        directory = save.lockPath().parent_path();
        for (int i = 0; i < 6; ++i) {
            auto edit = doc.begin("change");
            edit.setKeyValue(*doc.handle(doc.roots().front()), "message", std::to_string(launch * 6 + i));
            edit.commit(false);
            expected.push_back(doc.write().value());
            REQUIRE(save.snapshot(doc).value());
            REQUIRE(save.wait());
        }
    }
    const auto paths = vmfs(dir.path);
    REQUIRE(paths.size() == 10);
    std::set<std::string> actual;
    for (const auto& path : paths) actual.insert(read(path));
    CHECK(actual == std::set<std::string>(expected.end() - 10, expected.end()));
}

TEST_CASE("autosave same filename in different source directories remains isolated") {
    StateDir dir;
    editor::Autosave first(dir.path / "a/shared.vmf", dir.path / "state");
    editor::Autosave second(dir.path / "b/shared.vmf", dir.path / "state");
    editor::Autosave reopened(dir.path / "a/../a/shared.vmf", dir.path / "state");
    CHECK(first.lockPath().parent_path() != second.lockPath().parent_path());
    CHECK(first.lockPath().parent_path() == reopened.lockPath().parent_path());
    CHECK(first.lockPath() != reopened.lockPath());
}

TEST_CASE("autosave stale dead pid skips corrupt and truncated snapshots for newest valid one") {
    StateDir dir;
    // PID beyond either platform's PID range is deterministically dead; no race with
    // the machine's process allocator and no process needs to be killed.
    const auto dead = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_FALSE(editor::processRunning(dead));
    const auto lock = dir.path / "dead-session.lock";
    {
        std::ofstream out(lock);
        out << dead << "\n\"original.vmf\"\n\"003-dead-session.vmf\"\n";
    }
    const std::string good = "world { id 1 classname worldspawn }";
    for (const auto& [name, bytes] : {
             std::pair{"001-dead-session.vmf", good},
             std::pair{"002-dead-session.vmf", std::string("world { id 1")},
             std::pair{"003-dead-session.vmf", std::string("\"unterminated")},
             std::pair{"004-other-session.vmf", good}}) {
        std::ofstream out(dir.path / name, std::ios::binary);
        out << bytes;
    }
    const auto recoveries = editor::findRecoveries(dir.path);
    REQUIRE(recoveries);
    REQUIRE(recoveries->size() == 1);
    CHECK(recoveries->front().pid == dead);
    CHECK(recoveries->front().snapshot == dir.path / "001-dead-session.vmf");
    CHECK(read(recoveries->front().snapshot) == good);
    REQUIRE(editor::discardRecovery(recoveries->front()));
    CHECK_FALSE(fs::exists(lock));
    CHECK(vmfs(dir.path) == std::vector<fs::path>{dir.path / "004-other-session.vmf"});
}

TEST_CASE("autosave IO failure is a result and can be retried without throwing") {
    StateDir dir;
    auto parsed = vmf::parse("world { id 1 }");
    REQUIRE(parsed);
    document::Document doc(*parsed);
    editor::Autosave save("blocked.vmf", dir.path);
    const auto directory = save.lockPath().parent_path();
    // Replace the session directory with a regular file: deterministic ENOTDIR even
    // when tests run as root, unlike chmod-based permission tests.
    fs::remove(save.lockPath());
    fs::remove(directory);
    { std::ofstream blocker(directory); blocker << "blocked"; }
    CHECK_NOTHROW(save.snapshot(doc));
    const auto failed = save.wait();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == ErrorCode::IoError);
    fs::remove(directory);
    fs::create_directories(directory);
    REQUIRE(save.snapshot(doc).value());
    REQUIRE(save.wait());
    REQUIRE(vmfs(dir.path).size() == 1);
    CHECK(read(vmfs(dir.path).front()) == doc.write().value());
}
