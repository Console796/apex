#pragma once

#include "compile/runner.hpp"

#include <deque>

namespace apex::compile::test {

struct Script {
    std::vector<std::string> lines;
    int code = 0;
    bool pending = false;
};

class ScriptedHandle final : public ProcessHandle {
public:
    explicit ScriptedHandle(Script script) : script_(std::move(script)) {}
    void setCallbacks(std::function<void(std::string)> line, std::function<void(int)> finished) override {
        line_ = std::move(line);
        finished_ = std::move(finished);
        if (!line_ && !finished_) return;
        if (!script_.pending) finish();
    }
    void finish() {
        for (const auto& text : script_.lines) if (line_) line_(text);
        code_ = script_.code;
        auto callback = finished_;
        if (callback) callback(*code_);
    }
    void terminate() override {
        terminated = true;
        code_ = -1;
        auto callback = finished_;
        if (callback) callback(-1);
    }
    std::optional<int> exitCode() const override { return code_; }
    bool terminated = false;
private:
    Script script_;
    std::optional<int> code_;
    std::function<void(std::string)> line_;
    std::function<void(int)> finished_;
};

class ScriptedRunner final : public ProcessRunner {
public:
    struct Call {
        std::string program;
        std::vector<std::string> args;
        std::filesystem::path cwd;
        Environment env;
    };
    std::deque<Script> scripts;
    std::vector<Call> calls;
    std::vector<ScriptedHandle*> handles;
    Result<std::unique_ptr<ProcessHandle>> start(const std::string& program,
        const std::vector<std::string>& args, const std::filesystem::path& cwd, const Environment& env) override {
        calls.push_back({program, args, cwd, env});
        if (scripts.empty()) return std::unexpected(Error{ErrorCode::Internal, "No scripted process"});
        auto handle = std::make_unique<ScriptedHandle>(std::move(scripts.front()));
        scripts.pop_front();
        handles.push_back(handle.get());
        return handle;
    }
};

} // namespace apex::compile::test
