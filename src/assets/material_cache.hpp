#pragma once

#include "assets/vmt.hpp"
#include "assets/vtf.hpp"

#include <optional>

namespace apex::assets {

struct CachedMaterial {
    Material material;
    Texture texture; // editor preview (viewport override, tool texture, or base texture)
    std::optional<Texture> texture2;
    std::optional<Texture> bumpTexture; // Linear data, independently of the VTF colour flag.
    std::optional<Texture> mraoTexture;
    std::optional<Texture> phongExponentTexture;
    std::optional<Texture> envmapTexture;        // Named $envmap cube; never for env_cubemap.
    std::optional<Texture> envmapMaskTexture;    // Linear data.
    std::optional<Texture> selfIllumMaskTexture; // Linear data.
};
enum class MaterialState { Unloaded, Loading, Ready, Failed };

// Single owner thread: construction, all calls, destruction, and JobPool::drainCompletions
// must occur on that thread. Vfs and JobPool must outlive this cache. VMT resolution and
// Vfs reads happen on the owner; owned VTF byte snapshots decode asynchronously on workers.
// Do not refresh/mutate Vfs during a cache call. Destroying the cache cancels its jobs;
// workers and undelivered completions never borrow the cache or Vfs.
class MaterialCache {
public:
    MaterialCache(const Vfs& vfs, JobPool& jobs, std::size_t budgetBytes = 256 * 1024 * 1024);
    ~MaterialCache();
    MaterialCache(const MaterialCache&) = delete;
    MaterialCache& operator=(const MaterialCache&) = delete;

    // Starts a load once, returns the checkerboard while loading/failed. Call again after
    // draining completions to get the result. Ready requests update LRU order.
    // Returned immutable snapshots remain valid after eviction/destruction. The budget
    // counts only cache-owned decoded bytes, not snapshots retained by callers or jobs.
    std::shared_ptr<const CachedMaterial> request(std::string_view name);
    MaterialState state(std::string_view name) const;
    std::optional<Error> error(std::string_view name) const;
    void setBudget(std::size_t bytes);
    std::size_t residentBytes() const;
    // Canonical requested paths, including failed loads, for diagnostics.
    std::vector<std::string> requestedNames() const;
    std::shared_ptr<const CachedMaterial> placeholder() const;
    // Synchronous, uncached load and decode of one VTF through the cache's Vfs (owner thread),
    // for small engine textures such as engine/defaultcubemap.
    Result<Texture> loadTexture(std::string_view name) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace apex::assets
