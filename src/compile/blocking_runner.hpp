#pragma once

#include "compile/runner.hpp"

namespace apex::compile {

// Runs each process to completion inside start() (stdout and stderr merged), then replays its lines and exit
// code when callbacks are registered. For headless tools and tests with no event loop; a job over it finishes
// before CompileJob::start returns, and terminate() has nothing left to stop.
class BlockingProcessRunner final : public ProcessRunner {
public:
    Result<std::unique_ptr<ProcessHandle>> start(const std::string& program, const std::vector<std::string>& args,
        const std::filesystem::path& cwd, const Environment& env) override;
};

} // namespace apex::compile
