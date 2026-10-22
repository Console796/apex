#include <doctest/doctest.h>

#include "editor/autosave.hpp"
#include "document/document.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
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
