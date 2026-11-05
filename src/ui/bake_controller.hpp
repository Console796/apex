#pragma once

#include "assets/vfs.hpp"
#include "core/job_pool.hpp"
#include "lighting/bake_session.hpp"
#include "render/lightmap_atlas.hpp"
#include "render/render_mirror.hpp"
#include "render/vrad_lightmaps.hpp"

#include <QString>
#include <chrono>
#include <set>

class QWidget;

namespace apex::ui {
void showBakeSettings(QWidget* parent);

// What the hybrid view anchors to (0.7.7): the last VRAD run's lightmaps with the exported, instance-expanded
// document they were compiled from (ids already expanded) and its instance origins.
struct HybridBaseline {
    std::shared_ptr<const document::Document> exported;
    std::vector<document::InstanceSolidOrigin> origins;
    render::VradLightmaps lightmaps;
    std::uint64_t revision = 0;
    std::chrono::steady_clock::time_point completed{};
};

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
    // Whether the mirror draws this bake's lightmaps (the VRAD view takes the slot otherwise), and the lattice
    // checker (render::densityRadiance) in place of the radiance.
    void setDisplayed(bool displayed);
    void setDensity(bool density);
    // Hybrid mode (0.7.7): `hybrid` is whether the pane driving the bake shows it; either change drops the
    // session chain so Apex-only and VRAD-seeded states never mix. Needs a baseline, which a VRAD run supplies.
    void setHybrid(bool hybrid);
    void setBaseline(std::shared_ptr<const HybridBaseline> baseline);
    QString status() const;
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
    // The session that produced maps_/radiance_, kept across invalidate() so its converged state can seed the
    // next one; the rebuild job takes its state (a retired session yields none, which means a full restart).
    std::shared_ptr<lighting::BakeSession> previous_;
    std::shared_ptr<render::BakedLightmaps> maps_;
    std::vector<std::vector<Vec3>> radiance_;
    lighting::BakeSessionOptions options_;
    std::uint64_t revision_ = 0;
    std::chrono::steady_clock::time_point changed_{};
    QString status_, progress_, device_ = QStringLiteral("auto"), deviceStatus_;
    std::shared_ptr<const HybridBaseline> baseline_;
    std::set<lighting::FaceKey> hybridDirty_; // Rebaked since the baseline (faces the edits dirtied, across the chain).
    std::size_t hybridFaces_ = 0;
    bool hybrid_ = false, hybridFull_ = false;
    unsigned incremental_ = 0; // Consecutive incremental restarts since the last full one.
    float scale_ = render::kDefaultBakedScale;
    bool displayed_ = true, density_ = false;
    bool active_ = false, busy_ = false, redraw_ = false, failed_ = false;
    QString failure(const QString& text) const; // "<Baked|Hybrid>: text".
    void dropChain();
    void accept(lighting::BakePublication publication);
};
} // namespace apex::ui
