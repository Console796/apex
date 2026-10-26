#pragma once

#include "assets/model_cache.hpp"
#include "editor/pick_index.hpp"

#include <map>

namespace apex::ui {

std::shared_ptr<const editor::PickModel> makePickModel(const assets::ModelData& model);

// Owner-thread cache; snapshots stay alive while an index uses them.
class ModelPickCache {
public:
    explicit ModelPickCache(assets::ModelCache& models) : models_(models) {}
    std::optional<editor::EntityPickShape> shape(const kv1::Node& entity);
    bool pump();
    // Pair with an index rebuild after ModelCache::clear().
    void clear() { entries_.clear(); }

private:
    struct Entry {
        std::shared_ptr<const assets::ModelData> source;
        std::shared_ptr<const editor::PickModel> pick;
    };
    assets::ModelCache& models_;
    std::map<std::string, Entry> entries_;
};

} // namespace apex::ui
