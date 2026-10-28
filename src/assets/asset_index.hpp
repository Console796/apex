#pragma once

#include "assets/vfs.hpp"
#include "core/kv1.hpp"

#include <chrono>
#include <set>

namespace apex::assets {

template <typename Kind> struct AssetIndexSnapshot {
    using Entry = typename Kind::Entry;
    explicit AssetIndexSnapshot(std::vector<Entry> values = {});
    std::vector<Entry> entries; // sorted case-insensitively by name
    const Entry* find(std::string_view name) const;
};
struct AssetFilter {
    std::string name; // substring or full-name glob
    // Unset: any source. Engaged nullopt: game. Engaged index: that addon.
    std::optional<std::optional<std::size_t>> source;
    std::optional<std::set<std::string>> usedInMap;
};

struct AssetIndexProgress {
    std::size_t done = 0, total = 0;
    bool finished = false;
};
// Owner thread, including destruction and drainCompletions. Vfs and JobPool must outlive
// calls. step reads bytes on the owner, workers parse owned snapshots. The walk owns the
// Vfs backing state: destruction/cancel is safe, but no Vfs refresh/mutation until
// walking() is false (or the pool has joined), even after destroying this builder.
template <typename Kind> class AssetIndex {
public:
    AssetIndex(const Vfs& vfs, JobPool& jobs,
               kv1::EscapeMode mode = kv1::EscapeMode::Strata);
    ~AssetIndex();
    AssetIndex(const AssetIndex&) = delete;
    AssetIndex& operator=(const AssetIndex&) = delete;
    // Cooperatively bounded reads; a single filesystem read can exceed the budget.
    // Pump this and JobPool::drainCompletions until progress().finished.
    void step(std::chrono::milliseconds budget = std::chrono::milliseconds(4));
    void cancel();
    bool walking() const;
    AssetIndexProgress progress() const;
    std::shared_ptr<const typename Kind::Snapshot> snapshot() const; // null until finished
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace apex::assets
