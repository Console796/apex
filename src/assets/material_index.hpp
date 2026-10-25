#pragma once

#include "assets/vmt.hpp"

#include <chrono>
#include <set>

namespace apex::assets {

struct MaterialIndexEntry {
    std::string name;
    std::string shader;
    std::vector<std::string> keywords;
    bool toolTexture = false;
    std::string previewTexture;
    std::optional<std::size_t> mount; // Vfs::provider: addon index, empty for game content
    std::string error;
};
struct MaterialIndexSnapshot {
    explicit MaterialIndexSnapshot(std::vector<MaterialIndexEntry> entries = {});
    std::vector<MaterialIndexEntry> entries; // sorted case-insensitively by name
    const MaterialIndexEntry* find(std::string_view name) const;
};
struct MaterialFilter {
    std::string name; // substring or full-name glob; keyword:word tokens match whole keywords
    std::string shader;
    bool toolTextures = true;
    // Unset: any source. Set: entries whose mount equals it (nullopt = game content, not an addon).
    std::optional<std::optional<std::size_t>> source;
    std::optional<std::set<std::string>> usedInMap; // material names, any spelling
};
std::vector<std::size_t> filterMaterials(const MaterialIndexSnapshot& snapshot,
                                       const MaterialFilter& filter);

struct MaterialIndexProgress {
    std::size_t done = 0, total = 0;
    bool finished = false;
};
// Owner thread, including destruction and drainCompletions. Vfs and JobPool must outlive
// calls. step reads bytes on the owner, workers parse owned snapshots. The walk owns the
// Vfs backing state: destruction/cancel is safe, but no Vfs refresh/mutation until
// walking() is false (or the pool has joined), even after destroying this builder.
class MaterialIndex {
public:
    MaterialIndex(const Vfs& vfs, JobPool& jobs,
                  kv1::EscapeMode mode = kv1::EscapeMode::Strata);
    ~MaterialIndex();
    MaterialIndex(const MaterialIndex&) = delete;
    MaterialIndex& operator=(const MaterialIndex&) = delete;
    // Cooperatively bounded reads; a single filesystem read can exceed the budget.
    // Pump this and JobPool::drainCompletions until progress().finished.
    void step(std::chrono::milliseconds budget = std::chrono::milliseconds(4));
    void cancel();
    bool walking() const;
    MaterialIndexProgress progress() const;
    std::shared_ptr<const MaterialIndexSnapshot> snapshot() const; // null until finished
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace apex::assets
