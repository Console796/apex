#include "assets/material_cache.hpp"

#include <exception>
#include <map>

namespace apex::assets {
struct MaterialCache::Impl {
    struct Entry {
        MaterialState state = MaterialState::Unloaded;
        std::shared_ptr<const CachedMaterial> data;
        std::optional<Error> error;
        JobHandle job;
        std::uint64_t touched = 0;
        std::size_t bytes = 0;
    };
    const Vfs& vfs;
    JobPool& jobs;
    std::size_t budget, resident = 0;
    std::uint64_t clock = 0;
    std::map<std::string, Entry> entries;
    std::shared_ptr<const CachedMaterial> placeholder;

    Impl(const Vfs& vfs_, JobPool& jobs_, std::size_t budget_)
        : vfs(vfs_), jobs(jobs_), budget(budget_),
          placeholder(std::make_shared<CachedMaterial>(Material{}, checkerboardTexture(), std::nullopt)) {}
    void evict() {
        while (resident > budget) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (it->second.data && (oldest == entries.end() || it->second.touched < oldest->second.touched)) {
                    oldest = it;
                }
            if (oldest == entries.end()) {
                break;
            }
            resident -= oldest->second.bytes;
            oldest->second.data.reset();
            oldest->second.bytes = 0;
            oldest->second.state = MaterialState::Unloaded;
        }
    }
};
MaterialCache::MaterialCache(const Vfs& vfs, JobPool& jobs, std::size_t budgetBytes)
    : impl_(std::make_shared<Impl>(vfs, jobs, budgetBytes)) {}
MaterialCache::~MaterialCache() {
    for (auto& [key, entry] : impl_->entries) {
        entry.job.cancel();
    }
}
std::shared_ptr<const CachedMaterial> MaterialCache::request(std::string_view name) {
    const auto key = materialPath(name);
    auto& entry = impl_->entries[key];
    entry.touched = ++impl_->clock;
    if (entry.data) {
        return entry.data;
    }
    if (entry.state != MaterialState::Unloaded) {
        return impl_->placeholder;
    }
    auto fail = [&](Error error) {
        entry.state = MaterialState::Failed;
        entry.error = std::move(error);
        return impl_->placeholder;
    };
    auto material = loadMaterial(impl_->vfs, key);
    if (!material) {
        return fail(material.error());
    }
    if (material->previewTexture().empty()) {
        return fail(Error{ErrorCode::NotFound, "Material has no preview texture: " + key});
    }
    auto bytes = impl_->vfs.open(texturePath(material->previewTexture()));
    if (!bytes) {
        return fail(bytes.error());
    }
    Blob bytes2;
    if (!material->baseTexture2.empty()) {
        auto second = impl_->vfs.open(texturePath(material->baseTexture2));
        if (!second) {
            return fail(second.error());
        }
        bytes2 = std::move(*second);
    }
    entry.state = MaterialState::Loading;
    entry.job = impl_->jobs.submit(Priority::Normal,
        [material = std::move(*material), bytes = std::move(*bytes), bytes2 = std::move(bytes2)]
        (CancelToken token) mutable -> Result<CachedMaterial> {
            try {
                if (token.cancelled()) {
                    return std::unexpected(Error{ErrorCode::Internal, "Load cancelled"});
                }
                auto texture = decodeTexture(bytes);
                if (!texture) {
                    return std::unexpected(texture.error());
                }
                CachedMaterial result{std::move(material), std::move(*texture), {}};
                if (!result.material.baseTexture2.empty()) {
                    auto second = decodeTexture(bytes2);
                    if (!second) {
                        return std::unexpected(second.error());
                    }
                    result.texture2 = std::move(*second);
                }
                return result;
            } catch (const std::exception& e) {
                return std::unexpected(Error{ErrorCode::Internal, e.what()});
            }
        },
        [weak = std::weak_ptr<Impl>(impl_), key](Result<CachedMaterial> result) {
            auto impl = weak.lock();
            if (!impl) {
                return;
            }
            auto& loaded = impl->entries.at(key);
            if (!result) {
                loaded.state = MaterialState::Failed;
                loaded.error = result.error().withContext(key);
                return;
            }
            loaded.bytes = result->texture.byteSize() + (result->texture2 ? result->texture2->byteSize() : 0);
            impl->resident += loaded.bytes;
            loaded.data = std::make_shared<CachedMaterial>(std::move(*result));
            loaded.state = MaterialState::Ready;
            impl->evict();
        });
    return impl_->placeholder;
}
MaterialState MaterialCache::state(std::string_view name) const {
    auto it = impl_->entries.find(materialPath(name));
    return it == impl_->entries.end() ? MaterialState::Unloaded : it->second.state;
}
std::optional<Error> MaterialCache::error(std::string_view name) const {
    auto it = impl_->entries.find(materialPath(name));
    return it == impl_->entries.end() ? std::nullopt : it->second.error;
}
void MaterialCache::setBudget(std::size_t bytes) {
    impl_->budget = bytes;
    impl_->evict();
}
std::size_t MaterialCache::residentBytes() const { return impl_->resident; }
std::shared_ptr<const CachedMaterial> MaterialCache::placeholder() const { return impl_->placeholder; }
} // namespace apex::assets
