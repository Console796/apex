#pragma once

#include "assets/material_index.hpp"
#include "assets/vtf.hpp"

namespace apex::assets {
enum class ThumbnailKind { Material, Model };
enum class ThumbnailState { Unloaded, Loading, Ready, Failed };
// Owner thread, including destruction and drainCompletions; Vfs and jobs outlive calls.
// Reads VTF bytes on owner, decodes owned bytes on workers. Workers and completions never
// borrow this cache or Vfs. Held thumbnails survive eviction; budget counts cache ownership.
class ThumbnailCache {
public:
    ThumbnailCache(const Vfs& vfs, JobPool& jobs,
                   std::shared_ptr<const MaterialIndexSnapshot> index,
                   std::size_t budgetBytes = 64 * 1024 * 1024);
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;
    // Sizes round up to powers of two, clamped to [1, 65536]. Null while loading/failed.
    static int sizeBucket(int size);
    std::shared_ptr<const Thumbnail> request(std::string_view materialName, int size);
    ThumbnailState state(std::string_view materialName, int size) const;
    std::optional<Error> error(std::string_view materialName, int size) const;
    // External CPU images share the material LRU and budget. request() touches resident images;
    // it never starts work for external kinds. insert() is for non-material kinds and accepts
    // a failed render result.
    std::shared_ptr<const Thumbnail> request(ThumbnailKind kind, std::string_view name, int size);
    ThumbnailState state(ThumbnailKind kind, std::string_view name, int size) const;
    std::optional<Error> error(ThumbnailKind kind, std::string_view name, int size) const;
    void insert(ThumbnailKind kind, std::string_view name, int size, Result<Thumbnail> image);
    void setBudget(std::size_t bytes);
    std::size_t residentBytes() const;
    // Set by completed loads and budget changes; consume after draining completions.
    bool takeChanged();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace apex::assets
