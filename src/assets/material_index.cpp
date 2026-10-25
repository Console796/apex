#include "assets/material_index.hpp"

#include "assets/path_expand.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <exception>
#include <sstream>

namespace apex::assets {
namespace {
std::string nameKey(std::string_view name) {
    auto path = materialPath(name);
    return path.substr(10, path.size() - 14);
}
std::vector<std::string> words(std::string text) {
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream in(toLowerAscii(text));
    std::vector<std::string> result;
    for (std::string word; in >> word;) result.push_back(std::move(word));
    return result;
}
}
MaterialIndexSnapshot::MaterialIndexSnapshot(std::vector<MaterialIndexEntry> values)
    : entries(std::move(values)) {
    std::ranges::sort(entries, [](const auto& a, const auto& b) { return ciLess(a.name, b.name); });
}
const MaterialIndexEntry* MaterialIndexSnapshot::find(std::string_view name) const {
    auto key = nameKey(name);
    auto it = std::lower_bound(entries.begin(), entries.end(), key,
        [](const auto& entry, const auto& value) { return ciLess(entry.name, value); });
    return it != entries.end() && ciEquals(it->name, key) ? &*it : nullptr;
}
std::vector<std::size_t> filterMaterials(const MaterialIndexSnapshot& snapshot, const MaterialFilter& filter) {
    std::vector<std::string> keywords;
    std::istringstream in(toLowerAscii(filter.name));
    std::string pattern;
    for (std::string word; in >> word;) {
        if (word.starts_with("keyword:")) keywords.push_back(word.substr(8));
        else { if (!pattern.empty()) pattern += ' '; pattern += word; }
    }
    std::set<std::string> used;
    if (filter.usedInMap)
        for (const auto& name : *filter.usedInMap) used.insert(nameKey(name));
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
        const auto& entry = snapshot.entries[i];
        const auto name = toLowerAscii(entry.name);
        if (!filter.toolTextures && entry.toolTexture) continue;
        if (!filter.shader.empty() && !ciEquals(filter.shader, entry.shader)) continue;
        if (filter.source && *filter.source != entry.mount) continue;
        if (filter.usedInMap && !used.contains(nameKey(entry.name))) continue;
        if (pattern.find_first_of("*?") != std::string::npos) {
            if (!detail::matches(pattern, name)) continue;
        } else if (name.find(pattern) == std::string::npos) continue;
        if (!std::ranges::all_of(keywords, [&](const auto& keyword) {
                return std::ranges::find(entry.keywords, keyword) != entry.keywords.end();
            })) continue;
        result.push_back(i);
    }
    return result;
}

struct MaterialIndex::Impl {
    using Texts = std::map<std::string, Result<std::string>>;
    struct Batch {
        std::vector<MaterialIndexEntry> entries;
        std::set<std::string> missing;
    };
    const Vfs& vfs;
    JobPool& jobs;
    kv1::EscapeMode mode;
    JobHandle walk, parse;
    bool listed = false, stopped = false, parsing = false;
    std::vector<std::string> paths, pending;
    std::size_t read = 0, next = 0;
    std::shared_ptr<Texts> texts = std::make_shared<Texts>();
    std::vector<MaterialIndexEntry> entries;
    MaterialIndexProgress progress;
    std::shared_ptr<const MaterialIndexSnapshot> snapshot;
    Impl(const Vfs& v, JobPool& j, kv1::EscapeMode m) : vfs(v), jobs(j), mode(m) {}
};
MaterialIndex::MaterialIndex(const Vfs& vfs, JobPool& jobs, kv1::EscapeMode mode)
    : impl_(std::make_shared<Impl>(vfs, jobs, mode)) {
    impl_->walk = jobs.submit(Priority::Low,
        [walk = vfs.walkTask("materials", "*.vmt")](CancelToken token) {
            return token.cancelled() ? std::vector<std::string>{} : walk();
        }, [weak = std::weak_ptr<Impl>(impl_)](std::vector<std::string> paths) {
            if (auto impl = weak.lock(); impl && !impl->stopped) {
                impl->paths = std::move(paths);
                impl->pending = impl->paths;
                impl->progress.total = impl->paths.size();
                impl->listed = true;
            }
        });
}
MaterialIndex::~MaterialIndex() { cancel(); }
void MaterialIndex::cancel() {
    impl_->stopped = true;
    // Let the owned walk finish: JobHandle::Cancelled does not mean a worker has exited.
    impl_->parse.cancel();
}
bool MaterialIndex::walking() const {
    auto state = impl_->walk.state();
    return state == JobState::Queued || state == JobState::Running;
}
MaterialIndexProgress MaterialIndex::progress() const { return impl_->progress; }
std::shared_ptr<const MaterialIndexSnapshot> MaterialIndex::snapshot() const { return impl_->snapshot; }
void MaterialIndex::step(std::chrono::milliseconds budget) {
    auto& s = *impl_;
    if (s.stopped || !s.listed || s.parsing || s.progress.finished) return;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (s.read < s.pending.size() && std::chrono::steady_clock::now() < deadline) {
        auto path = s.pending[s.read++];
        auto bytes = s.vfs.open(path);
        if (bytes) s.texts->emplace(path, std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        else s.texts->emplace(path, std::unexpected(bytes.error()));
    }
    if (s.read < s.pending.size()) return;
    if (s.next == s.paths.size()) {
        s.snapshot = std::make_shared<MaterialIndexSnapshot>(std::move(s.entries));
        s.texts.reset();
        s.progress.finished = true;
        return;
    }
    const auto end = std::min(s.next + 64, s.paths.size());
    std::vector<MaterialIndexEntry> batch;
    for (auto i = s.next; i < end; ++i) {
        auto path = s.vfs.displayPath(s.paths[i]);
        MaterialIndexEntry entry;
        entry.name = path.substr(10, path.size() - 14);
        entry.mount = s.vfs.provider(s.paths[i]);
        batch.push_back(std::move(entry));
    }
    s.parsing = true;
    s.parse = s.jobs.submit(Priority::Low,
        [texts = std::shared_ptr<const Impl::Texts>(s.texts), batch = std::move(batch), mode = s.mode]
        (CancelToken token) mutable {
            Impl::Batch result;
            const MaterialReader reader = [&](std::string_view path) -> Result<std::string> {
                auto key = lookupKey(path);
                auto it = texts->find(key);
                if (it != texts->end()) return it->second;
                result.missing.insert(key);
                return std::unexpected(Error{ErrorCode::NotFound, "Include not read: " + key});
            };
            for (auto& entry : batch) {
                if (token.cancelled()) break;
                try {
                    auto material = parseMaterial(reader, entry.name, mode);
                    if (!material) entry.error = material.error().what();
                    else {
                        entry.shader = toLowerAscii(material->shader);
                        entry.previewTexture = material->previewTexture();
                        entry.keywords = words(material->keywords);
                        entry.toolTexture = material->hasToolTexture;
                    }
                    entry.toolTexture |= toLowerAscii(entry.name).starts_with("tools/");
                } catch (const std::exception& e) { entry.error = e.what(); }
                result.entries.push_back(std::move(entry));
            }
            return result;
        }, [weak = std::weak_ptr<Impl>(impl_), end](Impl::Batch result) {
            auto impl = weak.lock();
            if (!impl) return;
            impl->parsing = false;
            impl->pending.assign(result.missing.begin(), result.missing.end());
            impl->read = 0;
            if (!result.missing.empty()) return; // Read dependencies, then retry this batch.
            for (auto& entry : result.entries) impl->entries.push_back(std::move(entry));
            impl->next = end;
            impl->progress.done = end;
        });
}
} // namespace apex::assets
