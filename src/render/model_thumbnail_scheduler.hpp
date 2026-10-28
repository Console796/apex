#pragma once

#include "assets/material_cache.hpp"
#include "assets/model_cache.hpp"
#include "assets/thumbnail_cache.hpp"
#include "render/model_thumbnail.hpp"

#include <chrono>
#include <functional>
#include <list>
#include <map>

namespace apex::render {

// Owner thread, including callbacks; dependencies outlive it. Slice D requests visible paths
// first, demotes offscreen requests with request(path, false), or drops them with drop().
// Loading requests remain queued; completions are drained by the application's JobPool pump.
//
// Per tick: starts model loads (workers do the parsing) up to Options::maxLoads in flight and renders
// thumbnails of models whose materials have settled, until Options::tickBudget is spent (a tick always
// does at least one unit of work, so a slow item can overrun it). A model's skin-0/body-0 materials are requested once it is ready and
// waited for (Options::materialWaitTicks); those still missing then draw flat, like failed ones.
class ModelThumbnailScheduler {
public:
    using Render = std::function<Result<assets::Thumbnail>(const assets::ModelData&, const ModelMaterials&)>;
    struct ModelSource {
        std::function<assets::ModelState(std::string_view)> state;
        std::function<std::shared_ptr<const assets::ModelData>(std::string_view)> request;
        std::function<std::optional<Error>(std::string_view)> error;
    };
    // `exists` takes the canonical "materials/....vmt" path. An empty source means flat thumbnails.
    struct MaterialSource {
        std::function<assets::MaterialState(std::string_view)> state;
        std::function<std::shared_ptr<const assets::CachedMaterial>(std::string_view)> request;
        std::function<bool(const std::string&)> exists;
    };
    struct Options {
        std::size_t maxLoads = 24;                             // Model loads plus material waits in flight.
        std::chrono::microseconds tickBudget{8000};            // Time after which a tick starts no more work.
        int materialWaitTicks = 300;                           // Ticks to wait for a model's materials.
    };
    ModelThumbnailScheduler(assets::ModelCache& models, assets::ThumbnailCache& thumbnails,
                            ModelThumbnailRenderer& renderer);
    ModelThumbnailScheduler(assets::ModelCache& models, assets::MaterialCache& materials,
                            std::function<bool(const std::string&)> materialExists,
                            assets::ThumbnailCache& thumbnails, ModelThumbnailRenderer& renderer);
    // Injection seams for synthetic sources and render counters in headless tests.
    ModelThumbnailScheduler(assets::ThumbnailCache& thumbnails, ModelSource models,
                            std::function<Result<assets::Thumbnail>(const assets::ModelData&)> render);
    ModelThumbnailScheduler(assets::ThumbnailCache& thumbnails, ModelSource models, MaterialSource materials,
                            Render render);
    ModelThumbnailScheduler(const ModelThumbnailScheduler&) = delete;
    ModelThumbnailScheduler& operator=(const ModelThumbnailScheduler&) = delete;
    Options& options() { return options_; }
    void request(std::string_view path, bool visible = true);
    void drop(std::string_view path);
    // Returns true only when a render was attempted. No work during interaction. Ready visible
    // entries precede offscreen ones.
    bool tick(bool interactionInProgress);
    std::size_t pending() const { return queue_.size(); }
private:
    struct Request {
        std::string path;
        bool visible;
        int waitTicks = -1; // >= 0 once the model is ready and its materials are being waited for.
        std::optional<std::vector<std::pair<std::int32_t, std::string>>> materials; // Range index, material.
    };
    // True when the model can render now; fills `out` with the materials that are ready. `started` is
    // set when a material load began; none begins unless `mayStart`.
    bool gatherMaterials(Request& request, const assets::ModelData& model, ModelMaterials& out,
                         bool mayStart, bool& started);
    assets::ThumbnailCache& thumbnails_;
    ModelSource models_;
    MaterialSource materials_;
    Render render_;
    Options options_;
    std::list<Request> queue_;
    std::map<std::string, std::list<Request>::iterator> requests_;
};

} // namespace apex::render
