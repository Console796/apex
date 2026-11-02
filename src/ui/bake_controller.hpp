#pragma once

#include "assets/vfs.hpp"
#include "core/job_pool.hpp"
#include "lighting/bake_session.hpp"
#include "render/lightmap_atlas.hpp"
#include "render/render_mirror.hpp"

#include <QString>
#include <chrono>

class QWidget;

namespace apex::ui {
void showBakeSettings(QWidget* parent);

// GUI-thread owner. Only immutable document copies and the exclusive session cross to jobs;
// completions are delivered by JobPool::drainCompletions, between viewport draws.
class BakeController {
public:
    BakeController(document::Document& document, std::shared_ptr<const assets::Vfs> vfs,
                   render::Context& context, render::TextureHeap& heap, render::RenderMirror& mirror,
                   JobPool& jobs);
    ~BakeController();
    void invalidate();
    bool update(bool active, DVec3 camera, const std::filesystem::path& map,
                const std::optional<std::filesystem::path>& extraRoot);
    QString status() const { return status_; }
    float scale() const { return scale_; }

private:
    document::Document& document_;
    std::shared_ptr<const assets::Vfs> vfs_;
    render::Context& context_;
    render::TextureHeap& heap_;
    render::RenderMirror& mirror_;
    JobPool& jobs_;
    JobHandle job_;
    std::shared_ptr<lighting::BakeSession> session_;
    std::shared_ptr<render::BakedLightmaps> maps_;
    std::vector<std::vector<Vec3>> radiance_;
    lighting::BakeSessionOptions options_;
    std::uint64_t revision_ = 0;
    std::chrono::steady_clock::time_point changed_{};
    QString status_;
    float scale_ = render::kDefaultBakedScale;
    bool active_ = false, busy_ = false, redraw_ = false, failed_ = false;
    void accept(lighting::BakePublication publication);
};
} // namespace apex::ui
