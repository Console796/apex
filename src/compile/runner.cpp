#include "compile/runner.hpp"

#include <algorithm>
#include <array>

namespace apex::compile {

CompileJob::CompileJob(CompileProfile profile, CompilePaths paths, ProcessRunner& runner, FileOps& files)
    : profile_(std::move(profile)), paths_(std::move(paths)), runner_(runner), files_(files),
      steps_(profile_.commands.size()), prepared_(steps_.size()), handles_(steps_.size()),
      cwd_(paths_.workDirectory) {}

CompileJob::~CompileJob() {
    for (auto& handle : handles_) {
        if (!handle) continue;
        handle->setCallbacks({}, {});
        if (status_ == Status::Running && !handle->exitCode()) handle->terminate();
    }
}

Result<void> CompileJob::start() {
    if (status_ != Status::Queued)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Compile job already started"});
    auto preflightCwd = cwd_;
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        const auto& command = profile_.commands[i];
        auto& step = prepared_[i];
        if (!command.enabled || (!profile_.runGameAfter && command.executable == "$game_exe") ||
            (!profile_.copyBspToMaps && command.special == SpecialCommand::Copy &&
             command.arguments.find("$bspdir") != std::string::npos)) {
            step.skip = true;
            continue;
        }
        auto args = expandArguments(command.arguments, paths_.variables);
        auto program = expandVariables(command.executable, paths_.variables);
        if (!args || !program) {
            auto error = !args ? args.error() : program.error();
            fail(i, error.message);
            return std::unexpected(error);
        }
        step.args = std::move(*args);
        step.program = std::move(*program);
        if (command.special) {
            if (*command.special == SpecialCommand::ChangeDirectory && step.args.size() == 1)
                preflightCwd = (preflightCwd / step.args.front()).lexically_normal();
            continue;
        }
        const auto executable = std::filesystem::path(step.program);
        const auto resolved = executable.is_absolute() ? executable : preflightCwd / executable;
        if (step.program.empty() || !files_.exists(resolved)) {
            if (command.executable == "$postcompiler_exe") {
                step.skip = true;
                steps_[i].message = "Warning: postcompiler missing: " + step.program;
                continue;
            }
            Error error{ErrorCode::NotFound, "Missing compile executable " + command.executable + ": " + resolved.string()};
            fail(i, error.message);
            return std::unexpected(error);
        }
        step.program = resolved.string();
        auto add = [&](const std::string& flag, const std::string& value) {
            if (std::ranges::find(step.args, flag) == step.args.end()) {
                step.args.insert(step.args.begin(), {flag, value});
            }
        };
        if (std::ranges::find(step.args, "-game") == step.args.end()) {
            const auto game = paths_.variables.find("gamedir");
            if (game == paths_.variables.end()) {
                Error error{ErrorCode::InvalidArgument, "Missing compile variable $gamedir"};
                fail(i, error.message);
                return std::unexpected(error);
            }
            add("-game", game->second);
        }
        const auto bsp = paths_.variables.find("bsp_exe");
        if (command.executable == "$bsp_exe" || (bsp != paths_.variables.end() && step.program == bsp->second))
            add("-instancepath", paths_.instanceRoot.string());
    }
    status_ = Status::Running;
    advance();
    return {};
}

void CompileJob::fail(std::size_t index, std::string message) {
    status_ = Status::Failed;
    steps_[index].status = Status::Failed;
    steps_[index].message = std::move(message);
    for (auto& step : steps_) if (step.status == Status::Queued) step.status = Status::Skipped;
}

void CompileJob::complete(std::size_t index, int code) {
    auto& step = steps_[index];
    step.exitCode = code;
    if (status_ != Status::Running || step.status != Status::Running) return;
    if (code != 0) {
        fail(index, "Compile process exited with code " + std::to_string(code));
        return;
    }
    step.status = Status::Ok;
    if (!advancing_) advance();
}

void CompileJob::advance() {
    advancing_ = true;
    struct Reset {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{advancing_};
    while (status_ == Status::Running && next_ < steps_.size()) {
        const auto i = next_++;
        const auto& command = profile_.commands[i];
        auto& result = steps_[i];
        const auto& step = prepared_[i];
        if (!command.special) result.program = step.program;
        if (step.skip) {
            result.status = Status::Skipped;
            continue;
        }
        result.status = Status::Running;
        if (command.special) {
            const std::array roots{paths_.workDirectory, paths_.bspDirectory, paths_.addonDirectory};
            auto outcome = runSpecialCommand(*command.special, step.args, cwd_, roots, files_);
            if (!outcome) { fail(i, outcome.error().message); return; }
            result.status = Status::Ok;
            continue;
        }
        Environment env;
#ifdef __linux__
        env["LD_LIBRARY_PATH"] = paths_.toolDirectory.string();
#endif
        auto handle = runner_.start(step.program, step.args, cwd_, env);
        if (!handle) { fail(i, handle.error().message); return; }
        handles_[i] = std::move(*handle);
        handles_[i]->setCallbacks([this, i](std::string line) { steps_[i].lines.push_back(std::move(line)); },
                                 [this, i](int code) { complete(i, code); });
        if (command.wait && result.status == Status::Running) return;
        if (result.status == Status::Running) result.status = Status::Ok;
    }
    if (status_ == Status::Running) status_ = Status::Ok;
}

void CompileJob::cancel() {
    if (status_ != Status::Running && status_ != Status::Queued) return;
    status_ = Status::Cancelled;
    for (auto& step : steps_) {
        if (step.status == Status::Running || step.status == Status::Queued) step.status = Status::Cancelled;
    }
    for (auto& handle : handles_) if (handle && !handle->exitCode()) handle->terminate();
}

} // namespace apex::compile
