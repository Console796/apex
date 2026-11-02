#pragma once

#include "compile/runner.hpp"
#include "document/instance_contents.hpp"
#include "editor/compile_snapshot.hpp"

namespace apex::editor {

struct VradOptions {
    // Extra vrad arguments. -noextra keeps a run short and matches the lattice tools/light_parity.py reads
    // (it adds -bounce 0 -aoscale 0 for its direct-light-only references; a round trip keeps VRAD's bounces).
    std::string flags = "-noextra";
    bool vis = true; // vvis -fast between vbsp and vrad
};

// One in-flight VRAD round trip (0.7.5 D5): the document is copied, its instances are expanded (so the solid
// and side ids are the ones the Apex bake sees), written to a fresh directory under the system temp
// directory, and compiled with vbsp, optional vvis -fast and vrad. The user's map, compile settings and the
// game directory are never written. Owner thread only: the runner delivers its callbacks there, and poll()
// folds the compile's status into state().
class VradRoundTrip {
public:
    enum class State { Idle, Running, Done, Failed, Cancelled };

    VradRoundTrip(compile::ProcessRunner& runner, compile::FileOps& files,
                  std::filesystem::path root = std::filesystem::temp_directory_path());
    ~VradRoundTrip();
    VradRoundTrip(const VradRoundTrip&) = delete;
    VradRoundTrip& operator=(const VradRoundTrip&) = delete;

    // Fails while a run is in flight. The previous run's directory is removed first.
    Result<void> start(const document::Document& document, const std::filesystem::path& map,
                       const CompileGamePaths& game, const VradOptions& options);
    void cancel();
    // Call from the owner thread's loop; returns the current state.
    State poll();
    State state() const { return state_; }
    bool running() const { return state_ == State::Running; }
    // The document revision the run was exported from; an edit since makes the result stale.
    std::uint64_t revision() const { return revision_; }
    bool stale(std::uint64_t current) const { return state_ != State::Idle && current != revision_; }
    // The compiled BSP once state() is Done.
    const std::filesystem::path& bsp() const { return bsp_; }
    // The exported copy, instances expanded: the solid and side ids the mapped lightmaps refer to. Null before a start.
    const document::Document* exported() const { return exported_.get(); }
    const std::vector<document::InstanceSolidOrigin>& origins() const { return origins_; }
    // Failure or cancellation text, with the tail of the failing tool's output.
    const std::string& message() const { return message_; }
    // Keeps the run's directory when this object goes (tools that inspect the output).
    void keepFiles() { keep_ = true; }
    const std::filesystem::path& directory() const { return directory_; }

private:
    void discard();

    compile::ProcessRunner& runner_;
    compile::FileOps& files_;
    std::filesystem::path root_, directory_, bsp_;
    std::unique_ptr<compile::CompileJob> job_;
    std::unique_ptr<document::Document> exported_;
    std::vector<document::InstanceSolidOrigin> origins_;
    std::string message_;
    std::uint64_t revision_ = 0;
    State state_ = State::Idle;
    bool keep_ = false;
};

} // namespace apex::editor
