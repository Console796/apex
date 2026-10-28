#include "ui/qprocess_runner.hpp"

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <deque>

#ifdef __linux__
#include <csignal>
#include <unistd.h>
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace apex::ui {
namespace {
struct ProcessState final : QObject {
    QProcess process;
    QByteArray pending;
    std::deque<std::string> lines;
    std::function<void(std::string)> line;
    std::function<void(int)> finished;
    std::optional<int> code;
    bool attached = true;
    bool stopping = false;
    bool pumping = false;
    bool completionDelivered = false;
    qint64 pid = 0;
#ifdef _WIN32
    HANDLE job = nullptr;
    ~ProcessState() override { if (job) CloseHandle(job); }
#endif
    void pump() {
        if (pumping) return;
        pumping = true;
        while (line && !lines.empty()) {
            auto text = std::move(lines.front());
            lines.pop_front();
            auto callback = line;
            callback(std::move(text));
        }
        if (code && finished && !completionDelivered) {
            completionDelivered = true;
            auto callback = finished;
            callback(*code);
        }
        pumping = false;
    }
    void read(bool flush = false) {
        pending += process.readAllStandardOutput();
        qsizetype end;
        while ((end = pending.indexOf('\n')) >= 0) {
            auto bytes = pending.left(end);
            pending.remove(0, end + 1);
            if (bytes.endsWith('\r')) bytes.chop(1);
            lines.push_back(bytes.toStdString());
        }
        if (flush && !pending.isEmpty()) {
            lines.push_back(pending.toStdString());
            pending.clear();
        }
        pump();
    }
};

class Handle final : public compile::ProcessHandle {
public:
    explicit Handle(ProcessState* state) : state_(state) {}
    ~Handle() override {
        state_->line = {};
        state_->finished = {};
        state_->attached = false;
        if (state_->code) state_->deleteLater();
    }
    void setCallbacks(std::function<void(std::string)> line, std::function<void(int)> finished) override {
        state_->line = std::move(line);
        state_->finished = std::move(finished);
        state_->pump();
    }
    void terminate() override {
        if (state_->stopping || state_->code) return;
        state_->stopping = true;
#ifdef __linux__
        const auto pid = static_cast<pid_t>(state_->pid);
        if (pid > 0) {
            ::kill(-pid, SIGTERM);
            // The shell may exit before resistant descendants. Keep escalation independent of it.
            QTimer::singleShot(2000, Qt::PreciseTimer, QCoreApplication::instance(), [pid] { ::kill(-pid, SIGKILL); });
        }
#elif defined(_WIN32)
        if (state_->job) TerminateJobObject(state_->job, 1);
#else
        state_->process.terminate();
        QTimer::singleShot(2000, state_, [state = state_] {
            if (!state->code) state->process.kill();
        });
#endif
    }
    std::optional<int> exitCode() const override { return state_->code; }
private:
    ProcessState* state_;
};
} // namespace

Result<std::unique_ptr<compile::ProcessHandle>> QProcessRunner::start(const std::string& program,
    const std::vector<std::string>& args, const std::filesystem::path& cwd, const compile::Environment& env) {
    auto state = std::make_unique<ProcessState>();
    auto& process = state->process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setWorkingDirectory(QString::fromStdU16String(cwd.u16string()));
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& [key, value] : env)
        environment.insert(QString::fromStdString(key), QString::fromStdString(value));
    process.setProcessEnvironment(environment);
#ifdef __linux__
    process.setChildProcessModifier([] { if (::setsid() < 0) _exit(127); });
#endif
#ifdef _WIN32
    state->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!state->job || !SetInformationJobObject(state->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return std::unexpected(Error{ErrorCode::IoError, "Cannot create compile process job"});
    // Assign before the primary thread can run and spawn descendants.
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_SUSPENDED;
    });
#endif
    auto* raw = state.get();
    QObject::connect(&process, &QProcess::readyReadStandardOutput, raw, [raw] { raw->read(); });
    QObject::connect(&process, &QProcess::finished, raw, [raw](int code, QProcess::ExitStatus status) {
        raw->read(true);
        raw->code = status == QProcess::NormalExit ? code : -1;
        raw->pump();
        if (!raw->attached) raw->deleteLater();
    });
    QStringList arguments;
    for (const auto& arg : args) arguments.push_back(QString::fromStdString(arg));
    process.start(QString::fromStdString(program), arguments);
    if (!process.waitForStarted())
        return std::unexpected(Error{ErrorCode::IoError, "Cannot start " + program + ": " + process.errorString().toStdString()});
    raw->pid = process.processId();
#ifdef _WIN32
    HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE,
                               static_cast<DWORD>(raw->pid));
    const bool assigned = child && AssignProcessToJobObject(raw->job, child);
    if (child) CloseHandle(child);
    bool resumed = false;
    HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (assigned && threads != INVALID_HANDLE_VALUE && Thread32First(threads, &entry)) {
        do {
            if (entry.th32OwnerProcessID != static_cast<DWORD>(raw->pid)) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (thread) {
                resumed = ResumeThread(thread) != static_cast<DWORD>(-1);
                CloseHandle(thread);
            }
        } while (Thread32Next(threads, &entry));
    }
    if (threads != INVALID_HANDLE_VALUE) CloseHandle(threads);
    if (!assigned || !resumed) {
        process.kill();
        process.waitForFinished();
        return std::unexpected(Error{ErrorCode::IoError, "Cannot assign compile process to job"});
    }
#endif
    state.release();
    return std::unique_ptr<compile::ProcessHandle>(new Handle(raw));
}

Result<void> QProcessRunner::startDetached(const std::string& program, const std::vector<std::string>& args,
    const std::filesystem::path& cwd, const compile::Environment& env) {
    QProcess process;
    process.setProgram(QString::fromStdString(program));
    QStringList arguments;
    for (const auto& arg : args) arguments.push_back(QString::fromStdString(arg));
    process.setArguments(arguments);
    process.setWorkingDirectory(QString::fromStdU16String(cwd.u16string()));
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& [key, value] : env)
        environment.insert(QString::fromStdString(key), QString::fromStdString(value));
    process.setProcessEnvironment(environment);
#ifdef __linux__
    process.setChildProcessModifier([] { if (::setsid() < 0) _exit(127); });
#endif
    // The child gets its own session (Linux) or console (Windows) and no job object, so closing Apex
    // leaves the game running.
    if (!process.startDetached())
        return std::unexpected(Error{ErrorCode::IoError, "Cannot start " + program + ": " + process.errorString().toStdString()});
    return {};
}

} // namespace apex::ui
