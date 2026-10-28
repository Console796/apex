#pragma once

#include "compile/file_ops.hpp"
#include "compile/variables.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace apex::compile {

using Environment = std::map<std::string, std::string>;

class ProcessHandle {
public:
    // Destroying a handle disconnects observation; terminate() explicitly stops the process tree.
    virtual ~ProcessHandle() = default;
    // Callbacks run on the job's owning thread. Registering must replay buffered output/completion.
    // Replacing with empty callbacks disconnects them, including any queued deliveries.
    virtual void setCallbacks(std::function<void(std::string)> line, std::function<void(int)> finished) = 0;
    virtual void terminate() = 0;
    virtual std::optional<int> exitCode() const = 0;
};

class ProcessRunner {
public:
    virtual ~ProcessRunner() = default;
    // env overrides the inherited environment; start errors do not return a handle.
    virtual Result<std::unique_ptr<ProcessHandle>> start(const std::string& program,
        const std::vector<std::string>& args, const std::filesystem::path& cwd, const Environment& env) = 0;
    // Starts a process that outlives its caller: no output is read, the exit code is never seen, and
    // cancelling or destroying a job does not stop it (0.5 D9, the game). The default drops a started handle.
    virtual Result<void> startDetached(const std::string& program, const std::vector<std::string>& args,
        const std::filesystem::path& cwd, const Environment& env) {
        auto handle = start(program, args, cwd, env);
        if (!handle) return std::unexpected(handle.error());
        return {};
    }
};

enum class Status { Queued, Running, Ok, Failed, Cancelled, Skipped };

struct StepResult {
    Status status = Status::Queued;
    std::string program; // the expanded executable, empty for special commands
    std::vector<std::string> lines;
    std::string message;
    std::optional<int> exitCode;
};

struct CompilePaths {
    Variables variables;
    std::filesystem::path workDirectory;
    std::filesystem::path bspDirectory;
    std::filesystem::path addonDirectory;
    std::filesystem::path toolDirectory;
    std::filesystem::path instanceRoot;
    std::filesystem::path contentGameDirectory; // Compile tools only; game launches keep $gamedir.
};

class CompileJob {
public:
    CompileJob(CompileProfile profile, CompilePaths paths, ProcessRunner& runner, FileOps& files);
    ~CompileJob();
    CompileJob(const CompileJob&) = delete;
    CompileJob& operator=(const CompileJob&) = delete;
    Result<void> start();
    void cancel();
    Status status() const { return status_; }
    const std::vector<StepResult>& steps() const { return steps_; }

private:
    struct Prepared {
        std::string program;
        std::vector<std::string> args;
        bool skip = false;
    };
    void advance();
    void complete(std::size_t index, int code);
    void fail(std::size_t index, std::string message);

    CompileProfile profile_;
    CompilePaths paths_;
    ProcessRunner& runner_;
    FileOps& files_;
    Status status_ = Status::Queued;
    std::vector<StepResult> steps_;
    std::vector<Prepared> prepared_;
    std::vector<std::unique_ptr<ProcessHandle>> handles_;
    std::filesystem::path cwd_;
    std::size_t next_ = 0;
    bool advancing_ = false;
};

} // namespace apex::compile
