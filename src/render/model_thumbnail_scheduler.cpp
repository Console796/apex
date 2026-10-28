#include "render/model_thumbnail_scheduler.hpp"

#include <algorithm>
#include <set>

namespace apex::render {
namespace {
constexpr auto kind = assets::ThumbnailKind::Model;
constexpr int size = ModelThumbnailRenderer::kSize;

ModelThumbnailScheduler::ModelSource modelSource(assets::ModelCache& models) {
    return {[&models](std::string_view path) { return models.state(path); },
            [&models](std::string_view path) { return models.request(path); },
            [&models](std::string_view path) { return models.error(path); }};
}
}

ModelThumbnailScheduler::ModelThumbnailScheduler(assets::ModelCache& models, assets::ThumbnailCache& thumbnails,
                                                 ModelThumbnailRenderer& renderer)
    : ModelThumbnailScheduler(thumbnails, modelSource(models),
          [&renderer](const assets::ModelData& model) { return renderer.render(model); }) {}
ModelThumbnailScheduler::ModelThumbnailScheduler(assets::ModelCache& models, assets::MaterialCache& materials,
                                                 std::function<bool(const std::string&)> materialExists,
                                                 assets::ThumbnailCache& thumbnails, ModelThumbnailRenderer& renderer)
    : ModelThumbnailScheduler(thumbnails, modelSource(models),
          {[&materials](std::string_view name) { return materials.state(name); },
           [&materials](std::string_view name) { return materials.request(name); },
           std::move(materialExists)},
          [&renderer](const assets::ModelData& model, const ModelMaterials& loaded) {
              return renderer.render(model, loaded);
          }) {}
ModelThumbnailScheduler::ModelThumbnailScheduler(assets::ThumbnailCache& thumbnails, ModelSource models,
    std::function<Result<assets::Thumbnail>(const assets::ModelData&)> render)
    : ModelThumbnailScheduler(thumbnails, std::move(models), {},
          [render = std::move(render)](const assets::ModelData& model, const ModelMaterials&) { return render(model); }) {}
ModelThumbnailScheduler::ModelThumbnailScheduler(assets::ThumbnailCache& thumbnails, ModelSource models,
                                                 MaterialSource materials, Render render)
    : thumbnails_(thumbnails), models_(std::move(models)), materials_(std::move(materials)),
      render_(std::move(render)) {}

void ModelThumbnailScheduler::request(std::string_view path, bool visible) {
    auto name = assets::modelPath(path);
    auto found = requests_.find(name);
    if (found == requests_.end()) {
        queue_.push_back({name, visible});
        found = requests_.emplace(name, std::prev(queue_.end())).first;
    } else if (found->second->visible == visible) return;
    auto entry = found->second;
    entry->visible = visible;
    queue_.splice(queue_.end(), queue_, entry);
    if (visible) {
        auto offscreen = std::ranges::find(queue_, false, &Request::visible);
        queue_.splice(offscreen, queue_, entry);
    }
}
void ModelThumbnailScheduler::drop(std::string_view path) {
    auto found = requests_.find(assets::modelPath(path));
    if (found == requests_.end()) return;
    queue_.erase(found->second);
    requests_.erase(found);
}

bool ModelThumbnailScheduler::gatherMaterials(Request& request, const assets::ModelData& model,
                                              ModelMaterials& out, bool mayStart, bool& started) {
    if (!materials_.state || !materials_.request) return true;
    if (!request.materials) {
        request.materials.emplace();
        std::set<std::int32_t> seen;
        for (const auto& range : assets::modelDrawRanges(model, 0, 0)) {
            if (!range.indexCount || !seen.insert(range.material).second) continue;
            auto name = assets::resolveModelMaterial(model, range.material, [&](const std::string& vmt) {
                return materials_.exists ? materials_.exists(vmt) : true;
            });
            if (!name.empty()) request.materials->emplace_back(range.material, std::move(name));
        }
    }
    bool settled = true;
    for (const auto& [index, name] : *request.materials) {
        switch (materials_.state(name)) {
        case assets::MaterialState::Ready:
            if (auto material = materials_.request(name)) out.emplace(index, ModelMaterial{name, std::move(material)});
            break;
        case assets::MaterialState::Unloaded:
            if (mayStart) { materials_.request(name); started = true; }
            [[fallthrough]];
        case assets::MaterialState::Loading: settled = false; break;
        case assets::MaterialState::Failed: break;
        }
    }
    if (settled) return true;
    if (request.waitTicks < 0) request.waitTicks = 0;
    return ++request.waitTicks > options_.materialWaitTicks; // Out of patience: draw what is there.
}

bool ModelThumbnailScheduler::tick(bool interactionInProgress) {
    if (interactionInProgress) return false;
    const auto start = std::chrono::steady_clock::now();
    std::size_t inFlight = 0;
    for (const auto& entry : queue_)
        if (entry.waitTicks >= 0 || models_.state(entry.path) == assets::ModelState::Loading) ++inFlight;
    bool rendered = false, worked = false;
    for (auto it = queue_.begin(); it != queue_.end();) {
        const auto budgetSpent = worked && std::chrono::steady_clock::now() - start >= options_.tickBudget;
        if (budgetSpent) break;
        const auto path = it->path;
        const auto thumbnailState = thumbnails_.state(kind, path, size);
        if (thumbnailState == assets::ThumbnailState::Ready || thumbnailState == assets::ThumbnailState::Failed) {
            if (thumbnailState == assets::ThumbnailState::Ready) thumbnails_.request(kind, path, size);
            requests_.erase(path);
            it = queue_.erase(it);
            continue;
        }
        if (thumbnailState == assets::ThumbnailState::Loading) { ++it; continue; }
        const auto modelState = models_.state(path);
        if (modelState == assets::ModelState::Loading) { ++it; continue; }
        if (modelState == assets::ModelState::Unloaded) {
            if (inFlight < options_.maxLoads) {
                models_.request(path);
                ++inFlight;
                worked = true;
            }
            ++it;
            continue;
        }
        if (modelState == assets::ModelState::Failed) {
            thumbnails_.insert(kind, path, size, std::unexpected(models_.error(path).value_or(
                Error{ErrorCode::NotFound, "Model failed to load"})));
            requests_.erase(path);
            it = queue_.erase(it);
            continue;
        }
        auto model = models_.request(path);
        if (!model) { ++it; continue; }
        ModelMaterials loaded;
        const bool wasWaiting = it->waitTicks >= 0;
        bool startedMaterial = false;
        if (!gatherMaterials(*it, *model, loaded, true, startedMaterial)) {
            worked |= startedMaterial;
            if (!wasWaiting && it->waitTicks >= 0) ++inFlight;
            ++it;
            continue;
        }
        if (wasWaiting && inFlight) --inFlight;
        thumbnails_.insert(kind, path, size, render_(*model, loaded));
        rendered = worked = true;
        requests_.erase(path);
        it = queue_.erase(it);
    }
    return rendered;
}

} // namespace apex::render
