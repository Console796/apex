#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#define APEX_TEST_PROGRESS_ENABLED 1
#endif

// Progress for tools/test_top.py. A direct run of a doctest binary (apex_tests, apex_ui_tests, ...) writes
// <dir>/<binary>/progress.json (rewritten atomically after every test case), tests.log (one line per finished
// case) and failures.log (one block per failed assert). <dir> is $APEX_TEST_PROGRESS_DIR, else build/test-progress
// in the source tree. Off under ctest (one process per case would overwrite itself), for query runs, and with
// APEX_TEST_PROGRESS=0. It rides along with the console reporter and does not replace it.
// Under --jobs each child writes its own slice's files into a scratch dir and the parent merges them into this one
// set (current = the longest-running case of those in flight).
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

fs::path selfExe() {
    char exe[4096];
    const auto n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    return n > 0 ? fs::path(std::string(exe, static_cast<size_t>(n))) : fs::path();
}

fs::path progressRoot() {
    const char* env = std::getenv("APEX_TEST_PROGRESS_DIR");
    return env && *env ? fs::path(env) : fs::path(__FILE__).parent_path().parent_path() / "build" / "test-progress";
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
        binary = selfExe().empty() ? "tests" : selfExe().filename().string();
        dir = progressRoot() / binary;
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

// False when the command line picks reporters or only queries: those runs own the output.
bool plainRun(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        for (const char* p : {"-r=", "--reporters", "-ltc", "-lts", "-ltcr", "-ltsr", "--list-", "-c", "--count", "-v", "--version", "-h", "--help"})
            if (a == p || (std::strlen(p) > 2 && a.rfind(p, 0) == 0 && (p[std::strlen(p) - 1] == '=' || p[std::strlen(p) - 1] == '-' || a[std::strlen(p)] == '=')))
                return false;
    }
    return true;
}

bool underCtest() { return std::getenv("DASHBOARD_TEST_FROM_CTEST") != nullptr; }

bool wantsProgress(int argc, char** argv) {
    if (underCtest()) return false;
    if (const char* off = std::getenv("APEX_TEST_PROGRESS"); off && std::strcmp(off, "0") == 0) return false;
    return plainRun(argc, argv);
}

// --jobs=N (or -j=N, or $APEX_TEST_JOBS outside ctest) runs the selected test cases in N child processes of this
// binary. doctest keeps its registry and counters in globals, so cases cannot share a process across threads;
// each child gets a contiguous --first/--last slice of the filtered case list. 0 means one job per hardware thread.
// Returns -1 when no parallel run was asked for; `rest` gets argv without the jobs flag.
long parseJobs(int argc, char** argv, std::vector<std::string>& rest) {
    long jobs = -1;
    rest.emplace_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* value = a.rfind("--jobs=", 0) == 0 ? argv[i] + 7 : a.rfind("-j=", 0) == 0 ? argv[i] + 3 : nullptr;
        if (!value) { rest.push_back(a); continue; }
        char* end = nullptr;
        jobs = std::strtol(value, &end, 10);
        if (end == value || *end || jobs < 0) { std::fprintf(stderr, "invalid job count: %s\n", a.c_str()); std::exit(2); }
    }
    if (jobs < 0 && !underCtest())
        if (const char* env = std::getenv("APEX_TEST_JOBS"); env && *env) jobs = std::strtol(env, nullptr, 10);
    if (jobs == 0) jobs = std::max(1u, std::thread::hardware_concurrency());
    return jobs;
}

// The failed-case count from a child's doctest summary (doctest counts every case it skipped past --first/--last
// as passing the filters, and tolerates failed assertions in expected-failure cases, so only this column is usable).
unsigned failedCases(const std::string& out) {
    unsigned a = 0, b = 0, c = 0;
    const size_t at = out.rfind("test cases:");
    return at != std::string::npos && std::sscanf(out.c_str() + at, "test cases: %u | %u passed | %u failed", &a, &b, &c) == 3 ? c : 0;
}

// The raw JSON token (quotes and escapes intact) of a field in a ProgressReporter progress.json.
std::string jsonField(const std::string& text, const std::string& key) {
    const std::string needle = "\"" + key + "\": ";
    const size_t at = text.find(needle);
    if (at == std::string::npos) return "";
    const size_t from = at + needle.size();
    size_t end = from;
    if (end < text.size() && text[end] == '"') {
        for (++end; end < text.size() && text[end] != '"'; ++end) if (text[end] == '\\') ++end;
        ++end;
    } else {
        while (end < text.size() && text[end] != ',' && text[end] != '\n') ++end;
    }
    return text.substr(from, end - from);
}

double jsonNumber(const std::string& text, const std::string& key) { return std::atof(jsonField(text, key).c_str()); }

std::string readFile(const fs::path& path, size_t from = 0) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    in.seekg(static_cast<std::streamoff>(from));
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Merges the per-slice progress that child processes write into the one set of files tools/test_top.py reads.
struct ProgressMerge {
    struct Slice {
        bool active = false;
        unsigned done = 0, failed = 0;
        long long asserts = 0, assertsFailed = 0;
        std::string current, currentFile, currentLine = "0", lastFailure;
        double currentElapsed = 0;
        size_t testsOffset = 0, failuresOffset = 0;
    };

    fs::path dir, childRoot;   // Where the merged files go; where slice i writes <childRoot>/s<i>/<binary>/.
    std::string binary;
    std::vector<Slice> slices;
    std::ofstream testsLog, failuresLog;
    unsigned total;
    long long startUnix;
    Clock::time_point start = Clock::now();
    std::mutex mutex;

    ProgressMerge(unsigned sliceCount, unsigned totalCases, fs::path scratch)
        : childRoot(std::move(scratch)), slices(sliceCount), total(totalCases) {
        binary = selfExe().filename().string();
        dir = progressRoot() / binary;
        std::error_code ec;
        fs::create_directories(dir, ec);
        testsLog.open(dir / "tests.log", std::ios::trunc);
        failuresLog.open(dir / "failures.log", std::ios::trunc);
        startUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        write("running");
    }

    fs::path sliceDir(unsigned i) const { return childRoot / ("s" + std::to_string(i)); }

    void begin(unsigned i) { std::lock_guard lock(mutex); slices[i].active = true; }

    // Reads slice i's files; whole new log lines are appended to the merged logs. `end` retires the slice.
    void poll(unsigned i, bool end) {
        std::lock_guard lock(mutex);
        pollLocked(i);
        if (end) { slices[i].active = false; slices[i].current.clear(); }
    }

    void pollLocked(unsigned i) {
        Slice& sl = slices[i];
        const fs::path childDir = sliceDir(i) / binary;
        if (const std::string j = readFile(childDir / "progress.json"); !j.empty()) {
            sl.done = static_cast<unsigned>(jsonNumber(j, "done"));
            sl.failed = static_cast<unsigned>(jsonNumber(j, "failed"));
            sl.asserts = static_cast<long long>(jsonNumber(j, "asserts"));
            sl.assertsFailed = static_cast<long long>(jsonNumber(j, "assertsFailed"));
            sl.current = jsonField(j, "current");
            if (sl.current == "\"\"") sl.current.clear();
            sl.currentFile = jsonField(j, "currentFile");
            sl.currentLine = jsonField(j, "currentLine");
            sl.currentElapsed = jsonNumber(j, "currentElapsed");
            if (const std::string f = jsonField(j, "lastFailure"); f != "\"\"") sl.lastFailure = f;
        }
        const auto append = [](const fs::path& from, size_t& offset, std::ofstream& to) {
            std::string text = readFile(from, offset);
            const size_t cut = text.rfind('\n');
            if (cut == std::string::npos) return;
            text.resize(cut + 1);
            offset += text.size();
            to << text << std::flush;
        };
        append(childDir / "tests.log", sl.testsOffset, testsLog);
        append(childDir / "failures.log", sl.failuresOffset, failuresLog);
    }

    void pollActive() {
        std::lock_guard lock(mutex);
        for (unsigned i = 0; i < slices.size(); ++i) if (slices[i].active) pollLocked(i);
        writeLocked("running");
    }

    void write(const char* state) { std::lock_guard lock(mutex); writeLocked(state); }

    void writeLocked(const char* state) {
        unsigned done = 0, failed = 0;
        long long asserts = 0, assertsFailed = 0;
        const Slice* longest = nullptr;
        std::string lastFailure = "\"\"";
        for (const Slice& sl : slices) {
            done += sl.done; failed += sl.failed; asserts += sl.asserts; assertsFailed += sl.assertsFailed;
            if (!sl.lastFailure.empty()) lastFailure = sl.lastFailure;
            if (!sl.current.empty() && (!longest || sl.currentElapsed > longest->currentElapsed)) longest = &sl; // Of the cases running now, the one running longest.
        }
        std::string j = "{\n";
        j += "  \"binary\": " + jsonString(binary) + ",\n";
        j += "  \"pid\": " + std::to_string(getpid()) + ",\n";
        j += "  \"state\": " + jsonString(state) + ",\n";
        j += "  \"started\": " + std::to_string(startUnix) + ",\n";
        j += "  \"elapsed\": " + std::to_string(std::chrono::duration<double>(Clock::now() - start).count()) + ",\n";
        j += "  \"total\": " + std::to_string(total) + ",\n";
        j += "  \"done\": " + std::to_string(done) + ",\n";
        j += "  \"failed\": " + std::to_string(failed) + ",\n";
        j += "  \"skipped\": 0,\n";
        j += "  \"asserts\": " + std::to_string(asserts) + ",\n";
        j += "  \"assertsFailed\": " + std::to_string(assertsFailed) + ",\n";
        j += "  \"current\": " + (longest ? longest->current : "\"\"") + ",\n";
        j += "  \"currentFile\": " + (longest ? longest->currentFile : "\"\"") + ",\n";
        j += "  \"currentLine\": " + (longest ? longest->currentLine : "0") + ",\n";
        j += "  \"currentElapsed\": " + std::to_string(longest ? longest->currentElapsed : 0.0) + ",\n";
        j += "  \"lastFailure\": " + lastFailure + "\n}\n";
        const fs::path tmp = dir / "progress.json.tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << j;
        }
        std::error_code ec;
        fs::rename(tmp, dir / "progress.json", ec);
    }
};

int runParallel(const std::vector<std::string>& args, unsigned jobs, unsigned total, bool progress) {
    const std::string exe = selfExe().string();
    if (exe.empty()) { std::fprintf(stderr, "cannot locate own executable\n"); return 2; }

    // Small slices keep the load balanced when a few cases (corpus, render) dominate.
    const unsigned slices = std::min(total, jobs * 8);
    const unsigned sliceSize = (total + slices - 1) / slices;
    const unsigned sliceCount = (total + sliceSize - 1) / sliceSize;
    jobs = std::min(jobs, sliceCount);

    std::error_code ec;
    const fs::path tmpRoot = fs::temp_directory_path(ec) / ("apex-tests-" + std::to_string(getpid()));
    std::vector<std::string> baseEnv;
    for (char** e = environ; *e; ++e) {
        const std::string v = *e;
        // Children run serially, with their own temp dir and their own progress dir (merged below).
        if (v.rfind("APEX_TEST_JOBS=", 0) && v.rfind("APEX_TEST_PROGRESS", 0) && v.rfind("TMPDIR=", 0)) baseEnv.push_back(v);
    }
    std::unique_ptr<ProgressMerge> merge;
    if (progress) merge = std::make_unique<ProgressMerge>(sliceCount, total, tmpRoot / "progress");

    std::atomic<unsigned> next{0};
    std::mutex mutex;
    unsigned badSlices = 0, failedTotal = 0, finished = 0;
    const auto begin = Clock::now();

    auto worker = [&](unsigned w) {
        const fs::path tmp = tmpRoot / ("job" + std::to_string(w));
        std::error_code wec;
        fs::create_directories(tmp, wec);
        for (unsigned i; (i = next++) < sliceCount;) {
            std::vector<std::string> envStrings = baseEnv;
            envStrings.push_back("TMPDIR=" + tmp.string());
            if (merge) envStrings.push_back("APEX_TEST_PROGRESS_DIR=" + merge->sliceDir(i).string());
            else envStrings.push_back("APEX_TEST_PROGRESS=0");
            std::vector<char*> envp;
            for (auto& e : envStrings) envp.push_back(e.data());
            envp.push_back(nullptr);
            if (merge) merge->begin(i);
            const unsigned first = i * sliceSize + 1, last = std::min(total, (i + 1) * sliceSize);
            std::vector<std::string> argStrings{exe};
            argStrings.insert(argStrings.end(), args.begin() + 1, args.end());
            argStrings.push_back("--first=" + std::to_string(first));
            argStrings.push_back("--last=" + std::to_string(last));
            std::vector<char*> argvv;
            for (auto& a : argStrings) argvv.push_back(a.data());
            argvv.push_back(nullptr);

            const auto t0 = Clock::now();
            std::string output;
            int status = -1;
            int fds[2];
            if (pipe2(fds, O_CLOEXEC) == 0) {
                posix_spawn_file_actions_t actions;
                posix_spawn_file_actions_init(&actions);
                posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
                posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
                pid_t pid = 0;
                const int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argvv.data(), envp.data());
                posix_spawn_file_actions_destroy(&actions);
                close(fds[1]);
                if (rc == 0) {
                    char buf[8192];
                    for (ssize_t got; (got = read(fds[0], buf, sizeof buf)) > 0;) output.append(buf, static_cast<size_t>(got));
                    int st = 0;
                    waitpid(pid, &st, 0);
                    status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
                }
                close(fds[0]);
            }
            const double seconds = std::chrono::duration<double>(Clock::now() - t0).count();
            if (merge) merge->poll(i, true);

            std::lock_guard lock(mutex);
            ++finished;
            failedTotal += failedCases(output);
            if (status != 0) ++badSlices;
            std::printf("[apex_tests] slice %u/%u (cases %u-%u) %s in %.1fs  [%u/%u done]\n", i + 1, sliceCount, first, last,
                        status == 0 ? "ok" : (status < 0 ? "FAILED TO START" : ("FAILED, exit " + std::to_string(status)).c_str()), seconds,
                        finished, sliceCount);
            if (status != 0) std::printf("%s", output.c_str());
            std::fflush(stdout);
        }
    };

    std::atomic<bool> running{true};
    std::thread monitor([&] {
        while (running) {
            if (merge) merge->pollActive();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    std::vector<std::thread> threads;
    for (unsigned w = 0; w < jobs; ++w) threads.emplace_back(worker, w);
    for (auto& t : threads) t.join();
    running = false;
    monitor.join();
    fs::remove_all(tmpRoot, ec);

    const bool ok = badSlices == 0 && failedTotal == 0;
    if (merge) merge->write(ok ? "passed" : "failed");
    std::printf("[apex_tests] %u jobs, %u slices, %.1fs\n[doctest] test cases: %u | %u passed | %u failed\n[doctest] Status: %s\n", jobs, sliceCount,
                std::chrono::duration<double>(Clock::now() - begin).count(), total, total - std::min(total, failedTotal),
                failedTotal, ok ? "SUCCESS!" : "FAILURE!");
    return ok ? 0 : 1;
}
} // namespace
#endif

int main(int argc, char** argv) {
#ifdef APEX_TEST_PROGRESS_ENABLED
    std::vector<std::string> plainArgs;
    if (const long jobs = parseJobs(argc, argv, plainArgs); jobs > 0 && plainRun(argc, argv)) {
        std::vector<char*> countArgs;
        for (auto& a : plainArgs) countArgs.push_back(a.data());
        char countFlag[] = "--count", countReporter[] = "--reporters=apex_count";
        countArgs.push_back(countFlag);
        countArgs.push_back(countReporter);
        doctest::Context counter;
        counter.applyCommandLine(static_cast<int>(countArgs.size()), countArgs.data());
        counter.run();
        if (g_total == 0) { std::printf("[apex_tests] no test cases selected\n"); return 0; }
        return runParallel(plainArgs, static_cast<unsigned>(jobs), g_total, wantsProgress(argc, argv));
    }
#endif
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
