#include "assets/thumbnail_cache.hpp"

#include "assets/model_cache.hpp"

#include <algorithm>
#include <cassert>
#include <list>
#include <map>
#include <tuple>

namespace apex::assets {
struct ThumbnailCache::Impl {
    using Key = std::tuple<ThumbnailKind, std::string, int>;
    struct Entry {
        ThumbnailState state = ThumbnailState::Unloaded;
        std::shared_ptr<const Thumbnail> data;
        std::optional<Error> error;
        bool oversized = false;
        JobHandle job;
        std::list<Key>::iterator lru;
    };
    const Vfs& vfs;
    JobPool& jobs;
    std::shared_ptr<const MaterialIndexSnapshot> index;
    std::size_t budget, resident = 0;
    bool changed = false;
    std::map<Key, Entry> entries;
    std::list<Key> lru; // Only resident entries; oldest first.
    Impl(const Vfs& v, JobPool& j, std::shared_ptr<const MaterialIndexSnapshot> i, std::size_t b)
        : vfs(v), jobs(j), index(std::move(i)), budget(b) {}
    void evict() {
        while (resident > budget && !lru.empty()) {
            auto& entry = entries.at(lru.front());
            resident -= entry.data->rgba.size();
            entry.data.reset();
            entry.state = ThumbnailState::Unloaded;
            lru.pop_front();
        }
    }
    static Key key(std::string_view name, int size, ThumbnailKind kind = ThumbnailKind::Material) {
        return {kind, kind == ThumbnailKind::Material ? materialPath(name) : modelPath(name), sizeBucket(size)};
    }
};
int ThumbnailCache::sizeBucket(int size) {
    size = std::clamp(size, 1, 65536);
    int bucket = 1;
    while (bucket < size) bucket *= 2;
    return bucket;
}
ThumbnailCache::ThumbnailCache(const Vfs& vfs, JobPool& jobs,
                               std::shared_ptr<const MaterialIndexSnapshot> index, std::size_t budgetBytes)
    : impl_(std::make_shared<Impl>(vfs, jobs, std::move(index), budgetBytes)) {}
ThumbnailCache::~ThumbnailCache() {
    for (auto& [key, entry] : impl_->entries) entry.job.cancel();
}
std::shared_ptr<const Thumbnail> ThumbnailCache::request(std::string_view materialName, int size) {
    auto key = Impl::key(materialName, size);
    auto& s = *impl_;
    auto& entry = s.entries[key];
    if (entry.data) {
        s.lru.splice(s.lru.end(), s.lru, entry.lru);
        return entry.data;
    }
    if (entry.state != ThumbnailState::Unloaded) return {};
    auto fail = [&](Error error) -> std::shared_ptr<const Thumbnail> {
        entry.state = ThumbnailState::Failed;
        entry.error = std::move(error);
        s.changed = true;
        return {};
    };
    auto material = s.index ? s.index->find(materialName) : nullptr;
    if (!material) return fail(Error{ErrorCode::NotFound, "Material not indexed: " + std::get<1>(key)});
    if (!material->error.empty()) return fail(Error{ErrorCode::ParseError, material->error});
    if (material->previewTexture.empty()) return fail(Error{ErrorCode::NotFound, "Material has no preview texture"});
    auto bytes = s.vfs.open(texturePath(material->previewTexture));
    if (!bytes) return fail(bytes.error());
    entry.state = ThumbnailState::Loading;
    entry.job = s.jobs.submit(Priority::Low,
        [bytes = std::move(*bytes), size = std::get<2>(key)](CancelToken token) -> Result<Thumbnail> {
            if (token.cancelled()) return std::unexpected(Error{ErrorCode::Internal, "Thumbnail cancelled"});
            return decodeThumbnail(bytes, size);
        }, [weak = std::weak_ptr<Impl>(impl_), key](Result<Thumbnail> result) {
            auto impl = weak.lock();
            if (!impl) return;
            auto& loaded = impl->entries.at(key);
            impl->changed = true;
            if (!result) {
                loaded.state = ThumbnailState::Failed;
                loaded.error = result.error().withContext(std::get<1>(key));
                return;
            }
            if (result->rgba.size() > impl->budget) { // Evicting it at once would reload it every repaint.
                loaded.state = ThumbnailState::Failed;
                loaded.oversized = true;
                loaded.error = Error{ErrorCode::Unsupported, "Thumbnail exceeds the cache budget: " + std::get<1>(key)};
                return;
            }
            loaded.data = std::make_shared<Thumbnail>(std::move(*result));
            loaded.state = ThumbnailState::Ready;
            impl->resident += loaded.data->rgba.size();
            impl->lru.push_back(key);
            loaded.lru = std::prev(impl->lru.end());
            impl->evict();
        });
    return {};
}
ThumbnailState ThumbnailCache::state(std::string_view name, int size) const {
    auto it = impl_->entries.find(Impl::key(name, size));
    return it == impl_->entries.end() ? ThumbnailState::Unloaded : it->second.state;
}
std::optional<Error> ThumbnailCache::error(std::string_view name, int size) const {
    auto it = impl_->entries.find(Impl::key(name, size));
    return it == impl_->entries.end() ? std::nullopt : it->second.error;
}
std::shared_ptr<const Thumbnail> ThumbnailCache::request(ThumbnailKind kind, std::string_view name, int size) {
    if (kind == ThumbnailKind::Material) return request(name, size);
    auto it = impl_->entries.find(Impl::key(name, size, kind));
    if (it == impl_->entries.end() || !it->second.data) return {};
    impl_->lru.splice(impl_->lru.end(), impl_->lru, it->second.lru);
    return it->second.data;
}
ThumbnailState ThumbnailCache::state(ThumbnailKind kind, std::string_view name, int size) const {
    auto it = impl_->entries.find(Impl::key(name, size, kind));
    return it == impl_->entries.end() ? ThumbnailState::Unloaded : it->second.state;
}
std::optional<Error> ThumbnailCache::error(ThumbnailKind kind, std::string_view name, int size) const {
    auto it = impl_->entries.find(Impl::key(name, size, kind));
    return it == impl_->entries.end() ? std::nullopt : it->second.error;
}
void ThumbnailCache::insert(ThumbnailKind kind, std::string_view name, int size, Result<Thumbnail> image) {
    assert(kind != ThumbnailKind::Material); // Material entries belong to the async decoder.
    auto key = Impl::key(name, size, kind);
    auto& s = *impl_;
    auto& entry = s.entries[key];
    if (entry.data) {
        s.resident -= entry.data->rgba.size();
        s.lru.erase(entry.lru);
    }
    entry = {};
    s.changed = true;
    if (image && (image->width <= 0 || image->height <= 0 ||
                  image->width > sizeBucket(size) || image->height > sizeBucket(size) ||
                  image->rgba.size() != std::size_t(image->width) * std::size_t(image->height) * 4))
        image = std::unexpected(Error{ErrorCode::InvalidArgument, "Invalid thumbnail image"});
    if (image && image->rgba.size() > s.budget) {
        entry.oversized = true;
        image = std::unexpected(Error{ErrorCode::Unsupported, "Thumbnail exceeds the cache budget"});
    }
    if (!image) {
        entry.state = ThumbnailState::Failed;
        entry.error = image.error().withContext(std::get<1>(key));
        return;
    }
    entry.data = std::make_shared<Thumbnail>(std::move(*image));
    entry.state = ThumbnailState::Ready;
    s.resident += entry.data->rgba.size();
    s.lru.push_back(key);
    entry.lru = std::prev(s.lru.end());
    s.evict();
}
void ThumbnailCache::setBudget(std::size_t bytes) {
    impl_->budget = bytes;
    for (auto& [key, entry] : impl_->entries)
        if (entry.oversized) entry = {};
    impl_->evict();
    impl_->changed = true;
}
std::size_t ThumbnailCache::residentBytes() const { return impl_->resident; }
bool ThumbnailCache::takeChanged() { return std::exchange(impl_->changed, false); }
} // namespace apex::assets
