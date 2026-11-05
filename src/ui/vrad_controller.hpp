#pragma once

#include "compile/file_ops.hpp"
#include "editor/vrad_roundtrip.hpp"
#include "render/vrad_lightmaps.hpp"

#include <QString>
#include <chrono>
#include <functional>
#include <memory>

class QWidget;

namespace apex::ui {

void showVradSettings(QWidget* parent);

// GUI-thread owner of the VRAD view's round trip (0.7.5 D5). It starts a run on entering the mode and on an
// explicit rerun(), folds the compile's progress into one status line, loads the finished BSP and hands the
// result to `publish` (the GPU upload). It owns no GPU state, so it runs under the offscreen platform with a
// fake process runner. A result stays on screen until the next run replaces it; an edit after the run only
// labels it stale. Settings (QSettings): vrad/flags, vrad/vis, vrad/displayScale.
class VradController {
public:
    using Loader = std::function<Result<render::VradLightmaps>(
        const std::filesystem::path&, const document::Document&, std::span<const document::InstanceSolidOrigin>)>;
    using Publish = std::function<Result<void>(render::VradLightmaps)>;
    enum class Phase { Idle, Running, Ready, Failed };
    // What the last successful run compiled (the hybrid view's anchor, 0.7.7): the exported, instance-expanded
    // document, its instance origins and the document revision. Replaced by the next success only.
    struct Baseline {
        std::shared_ptr<const document::Document> exported;
        std::vector<document::InstanceSolidOrigin> origins;
        std::uint64_t revision = 0;
        std::chrono::steady_clock::time_point completed{};
    };

    VradController(document::Document& document, compile::ProcessRunner& runner, Publish publish,
                   Loader loader = render::loadVradLightmaps);
    ~VradController();

    // Asks for a new run at the next update(); a run in flight is cancelled first.
    void rerun() { rerun_ = true; }
    void cancel();
    // Call each tick. `active`: some pane shows the VRAD or Hybrid view. True when the status or image changed.
    bool update(bool active, const std::filesystem::path& map, const editor::CompileGamePaths& game);

    Phase phase() const { return phase_; }
    bool stale() const { return phase_ == Phase::Ready && trip_.stale(document_.historyState()); }
    float scale() const { return scale_; }
    std::shared_ptr<const Baseline> baseline() const { return baseline_; }
    // One line for the pane's HUD, and the full text (the failing tool's log tail) for its tooltip.
    QString status() const;
    const QString& detail() const { return detail_; }

private:
    void start(const std::filesystem::path& map, const editor::CompileGamePaths& game);
    void finish();

    document::Document& document_;
    compile::FilesystemFileOps files_;
    editor::VradRoundTrip trip_;
    Publish publish_;
    Loader loader_;
    std::shared_ptr<const Baseline> baseline_;
    Phase phase_ = Phase::Idle;
    lighting::BspMapStats stats_;
    QString detail_;
    std::chrono::steady_clock::time_point started_{};
    float scale_ = 1.0f;
    bool active_ = false, rerun_ = false, attempted_ = false;
    std::chrono::seconds shown_{-1};
};

} // namespace apex::ui
