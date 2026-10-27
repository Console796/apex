#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#define APEX_TEST_PROGRESS_ENABLED 1
#endif

// Progress for tools/test_top.py. A direct run of a doctest binary (apex_tests, apex_ui_tests, ...) writes
// <dir>/<binary>/progress.json (rewritten atomically after every test case), tests.log (one line per finished
// case) and failures.log (one block per failed assert). <dir> is $APEX_TEST_PROGRESS_DIR, else build/test-progress
// in the source tree. Off under ctest (one process per case would overwrite itself), for query runs, and with
// APEX_TEST_PROGRESS=0. It rides along with the console reporter and does not replace it.
#ifdef APEX_TEST_PROGRESS_ENABLED
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

unsigned g_total = 0; // test cases passing the filters, from a counting pass before the run

struct CountReporter : doctest::IReporter {
    explicit CountReporter(const doctest::ContextOptions&) {}
    void report_query(const doctest::QueryData& q) override { if (q.run_stats) g_total = q.run_stats->numTestCasesPassingFilters; }
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_start(const doctest::TestCaseData&) override {}
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}

struct ProgressReporter : doctest::IReporter {
    fs::path dir;
    std::string binary, current, currentFile, state = "running";
    int currentLine = 0;
    unsigned done = 0, failed = 0, skipped = 0;
    int asserts = 0, assertsFailed = 0;
    std::string lastFailure;
    Clock::time_point start = Clock::now(), caseStart = start;
    long long startUnix = 0;
    std::ofstream testsLog, failuresLog;

    explicit ProgressReporter(const doctest::ContextOptions&) {
        char exe[4096];
        const auto n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        binary = n > 0 ? fs::path(std::string(exe, static_cast<size_t>(n))).filename().string() : "tests";
        const char* env = std::getenv("APEX_TEST_PROGRESS_DIR");
        const fs::path root = env && *env ? fs::path(env) : fs::path(__FILE__).parent_path().parent_path() / "build" / "test-progress";
        dir = root / binary;
        std::error_code ec;
        fs::create_directories(dir, ec);
        testsLog.open(dir / "tests.log", std::ios::trunc);
        failuresLog.open(dir / "failures.log", std::ios::trunc);
        startUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    double since(Clock::time_point t) const { return std::chrono::duration<double>(Clock::now() - t).count(); }

    void write() {
        std::string j = "{\n";
        j += "  \"binary\": " + jsonString(binary) + ",\n";
        j += "  \"pid\": " + std::to_string(getpid()) + ",\n";
        j += "  \"state\": " + jsonString(state) + ",\n";
        j += "  \"started\": " + std::to_string(startUnix) + ",\n";
        j += "  \"elapsed\": " + std::to_string(since(start)) + ",\n";
        j += "  \"total\": " + std::to_string(g_total) + ",\n";
        j += "  \"done\": " + std::to_string(done) + ",\n";
        j += "  \"failed\": " + std::to_string(failed) + ",\n";
        j += "  \"skipped\": " + std::to_string(skipped) + ",\n";
        j += "  \"asserts\": " + std::to_string(asserts) + ",\n";
        j += "  \"assertsFailed\": " + std::to_string(assertsFailed) + ",\n";
        j += "  \"current\": " + jsonString(current) + ",\n";
        j += "  \"currentFile\": " + jsonString(currentFile) + ",\n";
        j += "  \"currentLine\": " + std::to_string(currentLine) + ",\n";
        j += "  \"currentElapsed\": " + std::to_string(current.empty() ? 0.0 : since(caseStart)) + ",\n";
        j += "  \"lastFailure\": " + jsonString(lastFailure) + "\n}\n";
        const fs::path tmp = dir / "progress.json.tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << j;
        }
        std::error_code ec;
        fs::rename(tmp, dir / "progress.json", ec);
    }

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override { write(); }
    void test_run_end(const doctest::TestRunStats& s) override {
        current.clear();
        state = s.numTestCasesFailed || s.numAssertsFailed ? "failed" : "passed";
        write();
    }
    void test_case_start(const doctest::TestCaseData& d) override {
        current = d.m_name;
        currentFile = d.m_file.c_str();
        currentLine = d.m_line;
        caseStart = Clock::now();
        write();
    }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats& s) override {
        ++done;
        asserts += s.numAssertsCurrentTest;
        assertsFailed += s.numAssertsFailedCurrentTest;
        if (!s.testCaseSuccess) ++failed;
        testsLog << (s.testCaseSuccess ? "PASS " : "FAIL ") << s.seconds << "s " << current << "  [" << currentFile << ':' << currentLine << "]\n" << std::flush;
        current.clear();
        write();
    }
    void test_case_exception(const doctest::TestCaseException& e) override {
        lastFailure = std::string(e.is_crash ? "crash: " : "exception: ") + e.error_string.c_str();
        failuresLog << current << ": " << lastFailure << "\n" << std::flush;
    }
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData& a) override {
        if (!a.m_failed) return;
        lastFailure = std::string(a.m_file) + ':' + std::to_string(a.m_line) + ": " + a.m_expr;
        failuresLog << current << "\n  " << lastFailure << "\n" << std::flush;
        write();
    }
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override { ++skipped; }
};

DOCTEST_REGISTER_REPORTER("apex_count", 1, CountReporter);
DOCTEST_REGISTER_REPORTER("apex_progress", 1, ProgressReporter);

bool wantsProgress(int argc, char** argv) {
    if (std::getenv("DASHBOARD_TEST_FROM_CTEST")) return false;
    if (const char* off = std::getenv("APEX_TEST_PROGRESS"); off && std::strcmp(off, "0") == 0) return false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        // Anything that picks reporters or only queries owns the output.
        for (const char* p : {"-r=", "--reporters", "-ltc", "-lts", "-ltcr", "-ltsr", "--list-", "-c", "--count", "-v", "--version", "-h", "--help"})
            if (a == p || (std::strlen(p) > 2 && a.rfind(p, 0) == 0 && (p[std::strlen(p) - 1] == '=' || p[std::strlen(p) - 1] == '-' || a[std::strlen(p)] == '=')))
                return false;
    }
    return true;
}
} // namespace
#endif

int main(int argc, char** argv) {
    doctest::Context context;
    context.applyCommandLine(argc, argv);
#ifdef APEX_TEST_PROGRESS_ENABLED
    if (wantsProgress(argc, argv)) {
        std::vector<char*> countArgs(argv, argv + argc);
        char countFlag[] = "--count", countReporter[] = "--reporters=apex_count";
        countArgs.push_back(countFlag);
        countArgs.push_back(countReporter);
        doctest::Context counter;
        counter.applyCommandLine(static_cast<int>(countArgs.size()), countArgs.data());
        counter.run();
        context.setOption("reporters", "console,apex_progress");
    }
#endif
    return context.run();
}
