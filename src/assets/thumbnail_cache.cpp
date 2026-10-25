#include "assets/thumbnail_cache.hpp"

#include <algorithm>
#include <list>
#include <map>

namespace apex::assets {
struct ThumbnailCache::Impl {
    using Key = std::pair<std::string, int>;
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
    static Key key(std::string_view name, int size) { return {materialPath(name), sizeBucket(size)}; }
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
    if (!material) return fail(Error{ErrorCode::NotFound, "Material not indexed: " + key.first});
    if (!material->error.empty()) return fail(Error{ErrorCode::ParseError, material->error});
    if (material->previewTexture.empty()) return fail(Error{ErrorCode::NotFound, "Material has no preview texture"});
    auto bytes = s.vfs.open(texturePath(material->previewTexture));
    if (!bytes) return fail(bytes.error());
    entry.state = ThumbnailState::Loading;
    entry.job = s.jobs.submit(Priority::Low,
        [bytes = std::move(*bytes), size = key.second](CancelToken token) -> Result<Thumbnail> {
            if (token.cancelled()) return std::unexpected(Error{ErrorCode::Internal, "Thumbnail cancelled"});
            return decodeThumbnail(bytes, size);
        }, [weak = std::weak_ptr<Impl>(impl_), key](Result<Thumbnail> result) {
            auto impl = weak.lock();
            if (!impl) return;
            auto& loaded = impl->entries.at(key);
            impl->changed = true;
            if (!result) {
                loaded.state = ThumbnailState::Failed;
                loaded.error = result.error().withContext(key.first);
                return;
            }
            if (result->rgba.size() > impl->budget) { // Evicting it at once would reload it every repaint.
                loaded.state = ThumbnailState::Failed;
                loaded.oversized = true;
                loaded.error = Error{ErrorCode::Unsupported, "Thumbnail exceeds the cache budget: " + key.first};
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
