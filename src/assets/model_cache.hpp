#pragma once

#include "assets/model_bake.hpp"
#include "assets/vfs.hpp"

#include <memory>
#include <optional>

namespace apex::assets {

enum class ModelState { Unloaded, Loading, Ready, Failed };

// Same ownership rules as MaterialCache: one owner thread for construction, every call,
// destruction and JobPool::drainCompletions; Vfs and JobPool must outlive the cache. The
// three files are read through Vfs on the owner, then parsed and baked on a worker from owned
// byte snapshots. Workers and undelivered completions never borrow the cache or Vfs.
class ModelCache {
public:
    ModelCache(const Vfs& vfs, JobPool& jobs, std::size_t budgetBytes = 256 * 1024 * 1024);
    ~ModelCache();
    ModelCache(const ModelCache&) = delete;
    ModelCache& operator=(const ModelCache&) = delete;

    // Starts a load once. Returns null while the model is unloaded or loading (the caller keeps
    // drawing its FGD box), the model once ready, and errorModel() once it failed. Ready requests
    // update LRU order. Returned snapshots stay valid after eviction, clear and destruction. The
    // budget counts only cache-owned bytes.
    std::shared_ptr<const ModelData> request(std::string_view name);
    ModelState state(std::string_view name) const;
    std::optional<Error> error(std::string_view name) const; // Logged once per path when it first fails.
    // Drops every entry and cancels loads, for a mount change (addon toggle, Reload).
    void clear();
    void setBudget(std::size_t bytes);
    std::size_t residentBytes() const;
    // A small cube, one submesh with material -1 (no material); the draw tints it with the entity's colour.
    std::shared_ptr<const ModelData> errorModel() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// "models/props/foo.mdl" from any case, backslashes, or a missing extension.
std::string modelPath(std::string_view name);
std::size_t modelByteSize(const ModelData& model);

} // namespace apex::assets
