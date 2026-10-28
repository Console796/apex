#pragma once

#include "compile/runner.hpp"

namespace apex::ui {

class QProcessRunner final : public compile::ProcessRunner {
public:
    Result<std::unique_ptr<compile::ProcessHandle>> start(const std::string& program,
        const std::vector<std::string>& args, const std::filesystem::path& cwd,
        const compile::Environment& env) override;
};

} // namespace apex::ui
