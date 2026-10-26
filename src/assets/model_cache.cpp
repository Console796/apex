#include "assets/model_cache.hpp"

#include "core/log.hpp"
#include "core/paths.hpp"

#include <mdlpp/mdlpp.h>

#include <exception>
#include <map>

namespace apex::assets {
namespace {
ModelData makeErrorModel() {
    constexpr float h = 8.0f;
    ModelData model;
    // Eight corners; faces share vertices, so normals point from the centre (flat shading comes from the draw).
    for (int i = 0; i < 8; ++i) {
        const Vec3 position{i & 1 ? h : -h, i & 2 ? h : -h, i & 4 ? h : -h};
        model.vertices.push_back({position, glm::normalize(position), {i & 1 ? 1.0f : 0.0f, i & 2 ? 1.0f : 0.0f}});
    }
    constexpr std::uint32_t quads[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3},
                                           {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
    for (const auto& quad : quads) {
        for (const auto corner : {0, 1, 2, 0, 2, 3}) model.indices.push_back(quad[corner]);
    }
    model.submeshes.push_back({0, static_cast<std::uint32_t>(model.indices.size()), 0, 0, 0, -1});
    model.bodyParts.push_back({"error", 1, {{"error", {}}}});
    model.bodyParts[0].choices[0].bounds.expand(DVec3{-h});
    model.bodyParts[0].choices[0].bounds.expand(DVec3{h});
    model.bounds = model.bodyParts[0].choices[0].bounds;
    return model;
}
} // namespace

std::string modelPath(std::string_view name) {
    auto path = lookupKey(name);
    if (!path.ends_with(".mdl")) path += ".mdl";
    return path;
}

std::size_t modelByteSize(const ModelData& model) {
    std::size_t bytes = model.vertices.size() * sizeof(ModelVertex) + model.indices.size() * sizeof(std::uint32_t) +
                        model.submeshes.size() * sizeof(ModelSubmesh);
    for (const auto& skin : model.skins) bytes += sizeof(skin) + skin.size() * sizeof(std::int16_t);
    for (const auto& name : model.materialNames) bytes += sizeof(name) + name.size();
    for (const auto& directory : model.materialDirectories) bytes += sizeof(directory) + directory.size();
    for (const auto& part : model.bodyParts) {
        bytes += sizeof(part) + part.name.size();
        for (const auto& choice : part.choices) bytes += sizeof(choice) + choice.name.size();
    }
    return bytes;
}

struct ModelCache::Impl {
    struct Entry {
        ModelState state = ModelState::Unloaded;
        std::shared_ptr<const ModelData> data;
        std::optional<Error> error;
        JobHandle job;
        std::uint64_t touched = 0;
        std::size_t bytes = 0;
    };
    const Vfs& vfs;
    JobPool& jobs;
    std::size_t budget, resident = 0;
    std::uint64_t clock = 0, generation = 0;
    std::map<std::string, Entry> entries;
    std::shared_ptr<const ModelData> errorModel = std::make_shared<ModelData>(makeErrorModel());

    Impl(const Vfs& vfs_, JobPool& jobs_, std::size_t budget_) : vfs(vfs_), jobs(jobs_), budget(budget_) {}
    void fail(const std::string& key, Entry& entry, Error error) {
        entry.state = ModelState::Failed;
        entry.error = error.withContext(key);
        log::warn("model {} failed: {}", key, entry.error->what());
    }
    // `keep` is the entry that just finished loading: a model larger than the budget stays until the next
    // load or a smaller budget, instead of reloading on every request.
    void evict(const Entry* keep = nullptr) {
        while (resident > budget) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (it->second.data && &it->second != keep &&
                    (oldest == entries.end() || it->second.touched < oldest->second.touched))
                    oldest = it;
            if (oldest == entries.end()) break;
            resident -= oldest->second.bytes;
            oldest->second.data.reset();
            oldest->second.bytes = 0;
            oldest->second.state = ModelState::Unloaded;
        }
    }
};

ModelCache::ModelCache(const Vfs& vfs, JobPool& jobs, std::size_t budgetBytes)
    : impl_(std::make_shared<Impl>(vfs, jobs, budgetBytes)) {}
ModelCache::~ModelCache() {
    for (auto& [key, entry] : impl_->entries) entry.job.cancel();
}

std::shared_ptr<const ModelData> ModelCache::request(std::string_view name) {
    const auto key = modelPath(name);
    auto& entry = impl_->entries[key];
    entry.touched = ++impl_->clock;
    if (entry.data) return entry.data;
    if (entry.state == ModelState::Failed) return impl_->errorModel;
    if (entry.state != ModelState::Unloaded) return nullptr;

    const auto stem = key.substr(0, key.size() - 4);
    auto mdl = impl_->vfs.open(key);
    if (!mdl) {
        impl_->fail(key, entry, mdl.error());
        return impl_->errorModel;
    }
    auto vtx = impl_->vfs.open(stem + ".dx90.vtx");
    if (!vtx) {
        impl_->fail(key, entry, vtx.error());
        return impl_->errorModel;
    }
    auto vvd = impl_->vfs.open(stem + ".vvd");
    if (!vvd) {
        impl_->fail(key, entry, vvd.error());
        return impl_->errorModel;
    }
    entry.state = ModelState::Loading;
    entry.job = impl_->jobs.submit(
        Priority::Normal,
        [mdl = std::move(*mdl), vtx = std::move(*vtx), vvd = std::move(*vvd)](CancelToken token) -> Result<ModelData> {
            try {
                if (token.cancelled()) return std::unexpected(Error{ErrorCode::Internal, "Load cancelled"});
                mdlpp::StudioModel source{};
                if (!source.open(mdl, vtx, vvd))
                    return std::unexpected(Error{ErrorCode::ParseError, "mdlpp could not open the model"});
                return bakeModel(source, mdl);
            } catch (const std::exception& e) {
                return std::unexpected(Error{ErrorCode::ParseError, std::string("mdlpp exception: ") + e.what()});
            }
        },
        [weak = std::weak_ptr<Impl>(impl_), key, generation = impl_->generation](Result<ModelData> result) {
            auto impl = weak.lock();
            if (!impl || impl->generation != generation) return;
            const auto it = impl->entries.find(key);
            if (it == impl->entries.end()) return;
            auto& loaded = it->second;
            if (!result) {
                impl->fail(key, loaded, result.error());
                return;
            }
            loaded.bytes = modelByteSize(*result);
            impl->resident += loaded.bytes;
            loaded.data = std::make_shared<const ModelData>(std::move(*result));
            loaded.state = ModelState::Ready;
            loaded.touched = ++impl->clock;
            impl->evict(&loaded);
        });
    return nullptr;
}

ModelState ModelCache::state(std::string_view name) const {
    const auto it = impl_->entries.find(modelPath(name));
    return it == impl_->entries.end() ? ModelState::Unloaded : it->second.state;
}
std::optional<Error> ModelCache::error(std::string_view name) const {
    const auto it = impl_->entries.find(modelPath(name));
    return it == impl_->entries.end() ? std::nullopt : it->second.error;
}
void ModelCache::clear() {
    for (auto& [key, entry] : impl_->entries) entry.job.cancel();
    impl_->entries.clear();
    impl_->resident = 0;
    ++impl_->generation;
}
void ModelCache::setBudget(std::size_t bytes) {
    impl_->budget = bytes;
    impl_->evict();
}
std::size_t ModelCache::residentBytes() const { return impl_->resident; }
std::shared_ptr<const ModelData> ModelCache::errorModel() const { return impl_->errorModel; }

} // namespace apex::assets
