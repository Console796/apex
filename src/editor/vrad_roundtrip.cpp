#include "editor/vrad_roundtrip.hpp"

#include "editor/bake_instances.hpp"

#include <atomic>
#include <chrono>
#include <system_error>

namespace apex::editor {
namespace {
std::string uniqueName() {
    static std::atomic<unsigned> counter{0};
    return "apex-vrad-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
           std::to_string(counter++);
}

compile::CompileProfile profileFor(const VradOptions& options) {
    const std::string map = "-game $gamedir $path/$file";
    compile::CompileProfile profile;
    profile.name = "VRAD round trip";
    profile.runGameAfter = false;
    profile.copyBspToMaps = false;
    profile.commands.push_back({"$bsp_exe", map});
    if (options.vis) profile.commands.push_back({"$vis_exe", "-fast " + map});
    profile.commands.push_back({"$light_exe", (options.flags.empty() ? "" : options.flags + " ") + map});
    return profile;
}

std::string failureText(const compile::CompileJob& job) {
    for (const auto& step : job.steps()) {
        if (step.status != compile::Status::Failed) continue;
        auto text = step.message;
        const auto first = step.lines.size() > 4 ? step.lines.size() - 4 : 0;
        for (auto i = first; i < step.lines.size(); ++i) text += "\n" + step.lines[i];
        return text;
    }
    return "VRAD round trip failed";
}
}

VradRoundTrip::VradRoundTrip(compile::ProcessRunner& runner, compile::FileOps& files, std::filesystem::path root)
    : runner_(runner), files_(files), root_(std::move(root)) {}

VradRoundTrip::~VradRoundTrip() {
    cancel();
    discard();
}

void VradRoundTrip::discard() {
    job_.reset();
    if (!directory_.empty() && !keep_) {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    directory_.clear();
    bsp_.clear();
    origins_.clear();
    exported_.reset();
}

Result<void> VradRoundTrip::start(const document::Document& document, const std::filesystem::path& map,
                                  const CompileGamePaths& game, const VradOptions& options) {
    if (running()) return std::unexpected(Error{ErrorCode::InvalidArgument, "A VRAD round trip is already running"});
    discard();
    state_ = State::Idle;
    message_.clear();
    revision_ = document.historyState();
    try {
        directory_ = std::filesystem::absolute(root_) / uniqueName();
        std::filesystem::create_directories(directory_);
        // The copy is expanded in place; the user's document is untouched.
        exported_ = document.snapshot();
        origins_ = expandBakeInstances(*exported_, map, game.instanceRoot);
        // vrad does not light the runtime classes; the exported copy is baked for the tool run.
        auto snapshot = writeCompileSnapshot(*exported_, map, directory_, game, {}, {.bakeRuntimeLights = true});
        if (!snapshot) {
            message_ = snapshot.error().what();
            state_ = State::Failed;
            return std::unexpected(snapshot.error());
        }
        bsp_ = snapshot->paths.workDirectory / (snapshot->paths.variables.at("file") + ".bsp");
        job_ = std::make_unique<compile::CompileJob>(profileFor(options), std::move(snapshot->paths), runner_, files_);
    } catch (const std::exception& error) {
        message_ = error.what();
        state_ = State::Failed;
        return std::unexpected(Error{ErrorCode::IoError, error.what()});
    }
    state_ = State::Running;
    if (auto started = job_->start(); !started) {
        message_ = started.error().what();
        state_ = State::Failed;
        return std::unexpected(started.error());
    }
    poll();
    return {};
}

void VradRoundTrip::cancel() {
    if (state_ != State::Running) return;
    job_->cancel();
    message_ = "Cancelled";
    state_ = State::Cancelled;
}

VradRoundTrip::State VradRoundTrip::poll() {
    if (state_ != State::Running || !job_) return state_;
    switch (job_->status()) {
    case compile::Status::Ok: state_ = State::Done; break;
    case compile::Status::Failed: message_ = failureText(*job_); state_ = State::Failed; break;
    case compile::Status::Cancelled: message_ = "Cancelled"; state_ = State::Cancelled; break;
    default: break;
    }
    return state_;
}

} // namespace apex::editor
