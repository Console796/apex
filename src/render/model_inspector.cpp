#include "render/model_inspector.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace apex::render {
namespace {
using Scheduler = ModelThumbnailScheduler;
}

ModelInspector::ModelInspector(assets::ModelCache& models, assets::MaterialCache& materials,
                               std::function<bool(const std::string&)> materialExists, ModelThumbnailRenderer& renderer)
    : models_{[&models](std::string_view path) { return models.state(path); },
              [&models](std::string_view path) { return models.request(path); },
              [&models](std::string_view path) { return models.error(path); }},
      materials_{[&materials](std::string_view name) { return materials.state(name); },
                 [&materials](std::string_view name) { return materials.request(name); }, std::move(materialExists)},
      render_([&renderer](const assets::ModelData& model, const ModelMaterials& loaded, const ModelView& view) {
          return renderer.renderView(model, loaded, view);
      }) {}
ModelInspector::ModelInspector(Scheduler::ModelSource models, Scheduler::MaterialSource materials, Render render)
    : models_(std::move(models)), materials_(std::move(materials)), render_(std::move(render)) {}

void ModelInspector::clear() {
    path_.clear();
    settled_ = static_cast<std::size_t>(-1);
    rendered_ = false;
    const auto revision = inspection_.revision + 1;
    inspection_ = {};
    inspection_.revision = revision;
}

void ModelInspector::fill(const assets::ModelData& model, const ModelView& view) {
    auto& stats = inspection_.stats;
    stats = {};
    stats.skinFamilies = model.skins.size();
    stats.submeshes = model.submeshes.size();
    for (const auto& part : model.bodyParts) {
        ModelInspectBodyPart info{part.name, part.base ? part.base : 1, {}};
        for (const auto& choice : part.choices) info.choices.push_back(choice.name);
        stats.bodyParts.push_back(std::move(info));
    }
    AABB bounds;
    std::map<std::int32_t, std::size_t> triangles;
    std::vector<std::int32_t> order;
    for (const auto& range : assets::modelDrawRanges(model, view.body, view.skin)) {
        if (!range.indexCount || std::uint64_t(range.firstIndex) + range.indexCount > model.indices.size()) continue;
        stats.triangles += range.indexCount / 3;
        if (!triangles.contains(range.material)) order.push_back(range.material);
        triangles[range.material] += range.indexCount / 3;
        for (auto i = range.firstIndex; i < range.firstIndex + range.indexCount; ++i)
            if (model.indices[i] < model.vertices.size()) bounds.expand(DVec3(model.vertices[model.indices[i]].position));
    }
    stats.vertices = model.vertices.size();
    if (!bounds.empty()) stats.size = bounds.max - bounds.min;
    for (const auto material : order) {
        ModelInspectMaterial info;
        info.triangles = triangles[material];
        info.name = assets::resolveModelMaterial(model, material, [&](const std::string& vmt) {
            return materials_.exists ? materials_.exists(vmt) : true;
        });
        info.resolved = !info.name.empty();
        if (!info.resolved)
            info.name = material >= 0 && static_cast<std::size_t>(material) < model.materialNames.size()
                ? model.materialNames[static_cast<std::size_t>(material)] : std::string{};
        info.state = info.resolved && materials_.state ? materials_.state(info.name) : assets::MaterialState::Failed;
        stats.materials.push_back(std::move(info));
    }
}

const ModelInspection& ModelInspector::update(std::string_view requested, const ModelView& view, bool interacting) {
    const auto path = requested.empty() ? std::string{} : assets::modelPath(requested);
    if (path.empty()) {
        if (!path_.empty() || inspection_.state != ModelInspection::State::Idle) clear();
        return inspection_;
    }
    if (path != path_) {
        clear();
        path_ = path;
        inspection_.state = ModelInspection::State::Loading;
    }
    const auto setState = [&](ModelInspection::State state, std::optional<Error> error = std::nullopt) {
        if (inspection_.state == state) return;
        inspection_.state = state;
        inspection_.error = std::move(error);
        ++inspection_.revision;
    };
    switch (models_.state(path)) {
    case assets::ModelState::Unloaded: models_.request(path); setState(ModelInspection::State::Loading); return inspection_;
    case assets::ModelState::Loading: setState(ModelInspection::State::Loading); return inspection_;
    case assets::ModelState::Failed:
        setState(ModelInspection::State::Failed, models_.error(path).value_or(Error{ErrorCode::NotFound, "Model failed to load"}));
        return inspection_;
    case assets::ModelState::Ready: break;
    }
    const auto model = models_.request(path);
    if (!model) return inspection_;
    // Start the current skin's materials and gather those that are ready.
    ModelMaterials loaded;
    std::size_t settled = 0;
    std::set<std::int32_t> seen;
    for (const auto& range : assets::modelDrawRanges(*model, view.body, view.skin)) {
        if (!range.indexCount || !seen.insert(range.material).second) continue;
        const auto name = assets::resolveModelMaterial(*model, range.material, [&](const std::string& vmt) {
            return materials_.exists ? materials_.exists(vmt) : true;
        });
        if (name.empty() || !materials_.state || !materials_.request) { ++settled; continue; }
        switch (materials_.state(name)) {
        case assets::MaterialState::Unloaded: materials_.request(name); break; // Starts the load.
        case assets::MaterialState::Loading: break;
        case assets::MaterialState::Failed: ++settled; break;
        case assets::MaterialState::Ready:
            if (auto material = materials_.request(name)) loaded.emplace(range.material, ModelMaterial{name, std::move(material)});
            ++settled;
            break;
        }
    }
    if (rendered_ && view == view_ && settled == settled_) return inspection_;
    if (interacting) return inspection_;
    view_ = view;
    settled_ = settled;
    rendered_ = true;
    fill(*model, view);
    auto image = render_(*model, loaded, view);
    ++inspection_.revision;
    if (image) {
        inspection_.image = std::move(*image);
        inspection_.state = ModelInspection::State::Ready;
        inspection_.error.reset();
    } else {
        inspection_.state = ModelInspection::State::Failed;
        inspection_.error = image.error();
    }
    return inspection_;
}

} // namespace apex::render
