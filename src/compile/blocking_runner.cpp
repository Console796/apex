#include "compile/blocking_runner.hpp"

#include <cstdio>

#ifndef _WIN32
#include <sys/wait.h>
#define APEX_POPEN popen
#define APEX_PCLOSE pclose
#else
#define APEX_POPEN _popen
#define APEX_PCLOSE _pclose
#endif

namespace apex::compile {
namespace {
std::string quoted(const std::string& text) {
#ifdef _WIN32
    return "\"" + text + "\"";
#else
    std::string out = "'";
    for (const char c : text) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
#endif
}

class FinishedHandle final : public ProcessHandle {
public:
    FinishedHandle(std::vector<std::string> lines, int code) : lines_(std::move(lines)), code_(code) {}
    void setCallbacks(std::function<void(std::string)> line, std::function<void(int)> finished) override {
        if (!line && !finished) return;
        if (line) for (auto& text : lines_) line(std::move(text));
        lines_.clear();
        if (finished) finished(code_);
    }
    void terminate() override {}
    std::optional<int> exitCode() const override { return code_; }

private:
    std::vector<std::string> lines_;
    int code_;
};
}

Result<std::unique_ptr<ProcessHandle>> BlockingProcessRunner::start(const std::string& program,
    const std::vector<std::string>& args, const std::filesystem::path& cwd, const Environment& env) {
    std::string command;
#ifdef _WIN32
    command = "cd /d " + quoted(cwd.string()) + " && ";
    for (const auto& [key, value] : env) command += "set " + quoted(key + "=" + value) + " && ";
#else
    command = "cd " + quoted(cwd.string()) + " && env";
    for (const auto& [key, value] : env) command += " " + quoted(key + "=" + value);
    command += " ";
#endif
    command += quoted(program);
    for (const auto& arg : args) command += " " + quoted(arg);
    command += " 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    FILE* pipe = APEX_POPEN(command.c_str(), "r");
    if (!pipe) return std::unexpected(Error{ErrorCode::IoError, "Cannot start " + program});
    std::vector<std::string> lines;
    std::string pending;
    char buffer[4096];
    while (const auto got = std::fread(buffer, 1, sizeof buffer, pipe)) {
        pending.append(buffer, got);
        std::size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            auto text = pending.substr(0, end);
            if (!text.empty() && text.back() == '\r') text.pop_back();
            lines.push_back(std::move(text));
            pending.erase(0, end + 1);
        }
    }
    if (!pending.empty()) lines.push_back(std::move(pending));
    int code = APEX_PCLOSE(pipe);
#ifndef _WIN32
    code = WIFEXITED(code) ? WEXITSTATUS(code) : -1;
#endif
    return std::unique_ptr<ProcessHandle>(std::make_unique<FinishedHandle>(std::move(lines), code));
}

} // namespace apex::compile
