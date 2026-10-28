#include "assets/asset_index.hpp"

#include "assets/material_index.hpp"
#include "assets/model_index.hpp"
#include "assets/sound_index.hpp"
#include "assets/particle_index.hpp"
#include "assets/path_expand.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"

#include <kvpp/DMX.h>
#include <mdlpp/structs/MDL.h>

#include <algorithm>
#include <climits>
#include <exception>
#include <map>
#include <sstream>
#include <type_traits>

namespace apex::assets {
namespace {
using Metadata = std::map<std::string, std::pair<std::string, std::optional<std::size_t>>>;

std::string assetPath(std::string_view name, std::string_view prefix, std::string_view extension = {}) {
    auto path = lookupKey(name);
    if (!path.starts_with(prefix)) path = std::string(prefix) + path;
    if (!extension.empty() && !path.ends_with(extension)) path += extension;
    return path;
}
std::string wavePath(std::string_view name) {
    // Source's sound-character modifiers may be combined, e.g. "*#ambient/file.wav".
    constexpr std::string_view modifiers = "*#@><^)(}`~!$?";
    while (!name.empty() && modifiers.find(name.front()) != std::string_view::npos) name.remove_prefix(1);
    return name.empty() ? std::string{} : assetPath(name, "sound/");
}
std::string textValue(const kv1::Node& node) {
    auto value = node.decodedValue();
    return value ? *value : std::string{};
}
std::vector<std::string> manifestFiles(const kv1::Node& node) {
    std::vector<std::string> result;
    for (const auto& child : node.children()) {
        if (ciEquals(child.key(), "file") || ciEquals(child.key(), "precache_file") ||
            ciEquals(child.key(), "preload_file")) {
            auto path = textValue(child);
            if (!path.empty() && path.front() == '!') path.erase(0, 1);
            result.push_back(lookupKey(path));
        } else if (child.kind() == kv1::NodeKind::Block) {
            auto nested = manifestFiles(child);
            result.insert(result.end(), nested.begin(), nested.end());
        }
    }
    return result;
}
std::vector<MaterialIndexEntry> parseIndexEntry(MaterialKind, const MaterialReader& reader,
    const std::string& path, const Metadata& metadata, kv1::EscapeMode mode) {
    MaterialIndexEntry entry;
    const auto& [display, mount] = metadata.at(path);
    entry.name = display.substr(10, display.size() - 14);
    entry.mount = mount;
    auto material = parseMaterial(reader, entry.name, mode);
    if (!material) entry.error = material.error().what();
    else {
        entry.shader = toLowerAscii(material->shader);
        entry.previewTexture = material->previewTexture();
        auto words = toLowerAscii(material->keywords);
        std::replace(words.begin(), words.end(), ',', ' ');
        std::istringstream in(words);
        for (std::string word; in >> word;) entry.keywords.push_back(std::move(word));
        entry.toolTexture = material->hasToolTexture;
    }
    entry.toolTexture |= toLowerAscii(entry.name).starts_with("tools/");
    return {std::move(entry)};
}
std::uint32_t headerWord(std::string_view bytes, std::size_t offset) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8);
    return result;
}
std::vector<ModelIndexEntry> parseIndexEntry(ModelKind, const MaterialReader& reader,
    const std::string& path, const Metadata& metadata, kv1::EscapeMode) {
    ModelIndexEntry entry;
    entry.name = path;
    entry.mount = metadata.at(path).second;
    auto bytes = reader(path);
    if (!bytes) entry.error = bytes.error().what();
    else if (bytes->size() < 408) entry.error = "Truncated MDL header";
    else {
        // The pinned mdlpp has only MDL::open, which otherwise traverses all tables. Feed it
        // the fixed header with table counts/offsets zeroed so indexing never parses geometry,
        // animation, header2 or skins. Counts remain available in the original header.
        std::array<std::byte, 408> header{};
        std::copy_n(reinterpret_cast<const std::byte*>(bytes->data()), 156, header.begin());
        mdlpp::MDL::MDL mdl{};
        if (!mdl.open(header.data(), header.size())) entry.error = "Invalid MDL header";
        else {
            entry.flags = static_cast<std::uint32_t>(mdl.flags);
            entry.staticProp = (entry.flags & mdlpp::MDL::MDL::FLAG_STATIC_PROP) != 0;
            const auto skins = headerWord(*bytes, 220), families = headerWord(*bytes, 224);
            const auto bodies = headerWord(*bytes, 232);
            if (skins > INT32_MAX || families > INT32_MAX || bodies > INT32_MAX)
                entry.error = "Negative MDL header count";
            else {
                entry.skinsPerFamily = skins;
                entry.skinFamilyCount = families;
                entry.bodyPartCount = bodies;
            }
            for (std::uint8_t i = 0; i < 3; ++i) {
                entry.hullMin[i] = mdl.hullMin[i]; entry.hullMax[i] = mdl.hullMax[i];
                entry.viewMin[i] = mdl.viewBBoxMin[i]; entry.viewMax[i] = mdl.viewBBoxMax[i];
            }
        }
    }
    return {std::move(entry)};
}
void collectWaves(const kv1::Node& node, std::vector<std::string>& waves) {
    for (const auto& child : node.children()) {
        if (ciEquals(child.key(), "wave") && child.kind() == kv1::NodeKind::Value) {
            auto wave = wavePath(textValue(child));
            if (!wave.empty()) waves.push_back(std::move(wave));
        } else if (ciEquals(child.key(), "rndwave")) collectWaves(child, waves);
    }
}
std::vector<SoundIndexEntry> parseIndexEntry(SoundKind, const MaterialReader& reader,
    const std::string& path, const Metadata& metadata, kv1::EscapeMode mode) {
    std::vector<SoundIndexEntry> result;
    if (path.starts_with("sound/")) {
        SoundIndexEntry entry;
        entry.name = path;
        entry.type = SoundIndexEntry::Type::Raw;
        entry.mount = metadata.at(path).second;
        entry.waves = {path};
        result.push_back(std::move(entry));
        return result;
    }
    std::set<std::string> visited;
    std::function<void(const std::string&, bool)> readScript;
    readScript = [&](const std::string& script, bool manifest) {
        if (!visited.insert(script).second) return;
        if (script.find_first_of("*?") != std::string::npos) {
            // Glob reads return an owned, NUL-separated Vfs listing, populated by step.
            auto listing = reader(script);
            if (listing) {
                std::istringstream paths(*listing);
                for (std::string file; std::getline(paths, file, '\0');) readScript(file, false);
            }
            return;
        }
        SoundIndexEntry failure;
        failure.name = script; failure.script = script; failure.type = SoundIndexEntry::Type::SoundScript;
        if (auto it = metadata.find(script); it != metadata.end()) failure.mount = it->second.second;
        auto bytes = reader(script);
        if (!bytes) { failure.error = bytes.error().what(); result.push_back(std::move(failure)); return; }
        auto doc = kv1::parse(*bytes, mode);
        if (!doc) { failure.error = doc.error().what(); result.push_back(std::move(failure)); return; }
        if (manifest) return;
        for (const auto& node : doc->root().children()) {
            if (node.kind() != kv1::NodeKind::Block) continue;
            SoundIndexEntry entry;
            entry.name = node.key(); entry.script = script; entry.type = SoundIndexEntry::Type::SoundScript;
            entry.mount = failure.mount;
            collectWaves(node, entry.waves);
            if (entry.waves.empty()) entry.error = "Soundscript entry has no waves";
            for (auto pair : {std::pair{"channel", &entry.channel}, {"volume", &entry.volume},
                              {"pitch", &entry.pitch}, {"soundlevel", &entry.soundlevel}})
                if (auto value = node.find(pair.first)) *pair.second = textValue(*value);
            result.push_back(std::move(entry));
        }
    };
    auto bytes = reader(path);
    if (!bytes) { readScript(path, true); return result; }
    auto doc = kv1::parse(*bytes, mode);
    if (!doc) { readScript(path, true); return result; }
    for (const auto& script : manifestFiles(doc->root())) readScript(script, false);
    return result;
}
std::vector<ParticleIndexEntry> parseIndexEntry(ParticleKind, const MaterialReader& reader,
    const std::string& path, const Metadata& metadata, kv1::EscapeMode mode) {
    std::vector<ParticleIndexEntry> result;
    auto bytes = reader(path);
    auto doc = bytes ? kv1::parse(*bytes, mode) : Result<kv1::Document>(std::unexpected(bytes.error()));
    if (!doc) {
        ParticleIndexEntry entry;
        entry.name = path; entry.pcf = path; entry.mount = metadata.at(path).second;
        entry.error = doc.error().what(); return {std::move(entry)};
    }
    std::set<std::string> visited;
    for (const auto& pcf : manifestFiles(doc->root())) {
        if (!visited.insert(pcf).second) continue;
        ParticleIndexEntry entry;
        entry.name = pcf; entry.pcf = pcf;
        if (auto it = metadata.find(pcf); it != metadata.end()) entry.mount = it->second.second;
        auto data = reader(pcf);
        if (!data) { entry.error = data.error().what(); result.push_back(std::move(entry)); continue; }
        try {
            kvpp::DMX dmx{std::string_view(*data)};
            if (!dmx) { entry.error = "Invalid particle DMX"; result.push_back(std::move(entry)); continue; }
            for (const auto& element : dmx.getElements()) {
                if (element.getType() != "DmeParticleSystemDefinition") continue;
                auto system = entry;
                system.name = element.getKey();
                result.push_back(std::move(system));
            }
        } catch (const std::exception& e) {
            entry.error = e.what(); result.push_back(std::move(entry));
        }
    }
    return result;
}
} // namespace

std::string ModelKind::key(std::string_view name) { return assetPath(name, "models/", ".mdl"); }
std::string SoundKind::key(std::string_view name) {
    const auto key = lookupKey(name);
    return key.ends_with(".wav") || key.ends_with(".mp3") ? wavePath(key) : key;
}
std::string ParticleKind::key(std::string_view name) { return toLowerAscii(name); }
std::function<std::vector<std::string>()> ModelKind::walk(const Vfs& vfs) {
    return vfs.walkTask("models", "*.mdl");
}
std::function<std::vector<std::string>()> SoundKind::walk(const Vfs& vfs) {
    return [walk = vfs.walkTask("sound"), manifest = vfs.exists("scripts/game_sounds_manifest.txt")] {
        auto paths = walk();
        std::erase_if(paths, [](const auto& path) { return !path.ends_with(".wav") && !path.ends_with(".mp3"); });
        if (manifest) paths.push_back("scripts/game_sounds_manifest.txt");
        return paths;
    };
}
std::function<std::vector<std::string>()> ParticleKind::walk(const Vfs& vfs) {
    const bool manifest = vfs.exists("particles/particles_manifest.txt");
    return [manifest] { return manifest ? std::vector<std::string>{"particles/particles_manifest.txt"}
                                       : std::vector<std::string>{}; };
}

template <typename Kind>
AssetIndexSnapshot<Kind>::AssetIndexSnapshot(std::vector<Entry> values) : entries(std::move(values)) {
    std::ranges::stable_sort(entries, [](const auto& a, const auto& b) { return ciLess(a.name, b.name); });
}
template <typename Kind>
const typename Kind::Entry* AssetIndexSnapshot<Kind>::find(std::string_view name) const {
    auto key = Kind::key(name);
    auto it = std::lower_bound(entries.begin(), entries.end(), key,
        [](const auto& entry, const auto& value) { return ciLess(entry.name, value); });
    return it != entries.end() && ciEquals(it->name, key) ? &*it : nullptr;
}
namespace {
template <typename Kind, typename Filter, typename Predicate>
std::vector<std::size_t> filterEntries(const AssetIndexSnapshot<Kind>& snapshot,
                                     const Filter& filter, Predicate predicate) {
    const auto pattern = toLowerAscii(filter.name);
    std::set<std::string> used;
    if (filter.usedInMap) for (const auto& name : *filter.usedInMap) used.insert(Kind::key(name));
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
        const auto& entry = snapshot.entries[i];
        if (filter.source && *filter.source != entry.mount) continue;
        if (filter.usedInMap && !used.contains(Kind::key(entry.name))) continue;
        const auto name = toLowerAscii(entry.name);
        if (pattern.find_first_of("*?") != std::string::npos) {
            if (!detail::matches(pattern, name)) continue;
        } else if (name.find(pattern) == std::string::npos) continue;
        if (predicate(entry)) result.push_back(i);
    }
    return result;
}
}
std::vector<std::size_t> filterModels(const ModelIndexSnapshot& snapshot, const ModelFilter& filter) {
    return filterEntries(snapshot, filter, [&](const auto& entry) { return !filter.staticPropOnly || entry.staticProp; });
}
std::vector<std::size_t> filterSounds(const SoundIndexSnapshot& snapshot, const SoundFilter& filter) {
    return filterEntries(snapshot, filter, [&](const auto& entry) { return !filter.type || entry.type == *filter.type; });
}
std::vector<std::size_t> filterParticles(const ParticleIndexSnapshot& snapshot, const ParticleFilter& filter) {
    return filterEntries(snapshot, filter, [](const auto&) { return true; });
}
template <typename Kind>
struct AssetIndex<Kind>::Impl {
    using Texts = std::map<std::string, Result<std::string>>;
    struct Batch {
        std::vector<typename Kind::Entry> entries;
        std::set<std::string> missing;
    };
    const Vfs& vfs;
    JobPool& jobs;
    kv1::EscapeMode mode;
    JobHandle walk, parse;
    bool listed = false, stopped = false, parsing = false;
    std::vector<std::string> paths, pending;
    std::set<std::string> dependencies;
    std::size_t read = 0, next = 0;
    std::shared_ptr<Metadata> metadata = std::make_shared<Metadata>();
    std::shared_ptr<Texts> texts = std::make_shared<Texts>();
    std::vector<typename Kind::Entry> entries;
    AssetIndexProgress progress;
    std::shared_ptr<const typename Kind::Snapshot> snapshot;
    Impl(const Vfs& v, JobPool& j, kv1::EscapeMode m) : vfs(v), jobs(j), mode(m) {}
};
template <typename Kind>
AssetIndex<Kind>::AssetIndex(const Vfs& vfs, JobPool& jobs, kv1::EscapeMode mode)
    : impl_(std::make_shared<Impl>(vfs, jobs, mode)) {
    impl_->walk = jobs.submit(Priority::Low,
        [walk = Kind::walk(vfs)](CancelToken token) {
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
template <typename Kind>
AssetIndex<Kind>::~AssetIndex() { cancel(); }
template <typename Kind>
void AssetIndex<Kind>::cancel() {
    impl_->stopped = true;
    // Let the owned walk finish: JobHandle::Cancelled does not mean a worker has exited.
    impl_->parse.cancel();
}
template <typename Kind>
bool AssetIndex<Kind>::walking() const {
    auto state = impl_->walk.state();
    return state == JobState::Queued || state == JobState::Running;
}
template <typename Kind>
AssetIndexProgress AssetIndex<Kind>::progress() const { return impl_->progress; }
template <typename Kind>
std::shared_ptr<const typename Kind::Snapshot> AssetIndex<Kind>::snapshot() const { return impl_->snapshot; }
template <typename Kind>
void AssetIndex<Kind>::step(std::chrono::milliseconds budget) {
    auto& s = *impl_;
    if (s.stopped || !s.listed || s.parsing || s.progress.finished) return;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (s.read < s.pending.size() && std::chrono::steady_clock::now() < deadline) {
        auto path = s.pending[s.read++];
        (*s.metadata)[path] = {s.vfs.displayPath(path), s.vfs.provider(path)};
        if constexpr (std::is_same_v<Kind, SoundKind>) {
            if (path.starts_with("sound/")) continue;
            if (path.find_first_of("*?") != std::string::npos) {
                const auto slash = path.rfind('/');
                const auto directory = slash == std::string::npos ? "" : path.substr(0, slash);
                const auto glob = path.substr(slash == std::string::npos ? 0 : slash + 1);
                std::string listing;
                for (const auto& file : s.vfs.walk(directory, glob)) {
                    (*s.metadata)[file] = {s.vfs.displayPath(file), s.vfs.provider(file)};
                    s.dependencies.insert(file);
                    listing += file;
                    listing += '\0';
                }
                s.texts->emplace(path, std::move(listing));
                continue;
            }
        }
        auto bytes = [&] {
            if constexpr (std::is_same_v<Kind, ModelKind>) return s.vfs.openPrefix(path, 408);
            return s.vfs.open(path);
        }();
        if (bytes) s.texts->emplace(path, std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        else s.texts->emplace(path, std::unexpected(bytes.error()));
    }
    if (s.read < s.pending.size()) return;
    if (s.next == s.paths.size()) {
        s.snapshot = std::make_shared<typename Kind::Snapshot>(std::move(s.entries));
        s.texts.reset();
        s.progress.finished = true;
        return;
    }
    const auto end = std::min(s.next + 64, s.paths.size());
    std::vector<typename Kind::Entry> batch;
    for (auto i = s.next; i < end; ++i) {
        typename Kind::Entry entry;
        entry.name = s.paths[i];
        entry.mount = s.vfs.provider(s.paths[i]);
        if constexpr (std::is_same_v<Kind, SoundKind>) {
            if (s.paths[i].starts_with("sound/") && !s.vfs.exists(s.paths[i]))
                entry.error = "Missing sound: " + s.paths[i];
        }
        batch.push_back(std::move(entry));
    }
    auto batchMetadata = std::make_shared<Metadata>();
    for (const auto& entry : batch)
        if (auto found = s.metadata->find(entry.name); found != s.metadata->end())
            batchMetadata->emplace(found->first, found->second);
    for (const auto& path : s.dependencies)
        if (auto found = s.metadata->find(path); found != s.metadata->end())
            batchMetadata->emplace(found->first, found->second);
    s.parsing = true;
    s.parse = s.jobs.submit(Priority::Low,
        [texts = std::shared_ptr<const typename Impl::Texts>(s.texts), batch = std::move(batch), mode = s.mode,
         metadata = std::shared_ptr<const Metadata>(std::move(batchMetadata))]
        (CancelToken token) mutable {
            typename Impl::Batch result;
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
                    auto parsed = parseIndexEntry(Kind{}, reader, entry.name, *metadata, mode);
                    for (auto& value : parsed) {
                        if (!entry.error.empty()) value.error = entry.error;
                        result.entries.push_back(std::move(value));
                    }
                    continue;
                } catch (const std::exception& e) { entry.error = e.what(); }
                result.entries.push_back(std::move(entry));
            }
            return result;
        }, [weak = std::weak_ptr<Impl>(impl_), end](typename Impl::Batch result) {
            auto impl = weak.lock();
            if (!impl || impl->stopped) return;
            impl->parsing = false;
            impl->pending.assign(result.missing.begin(), result.missing.end());
            impl->dependencies.insert(result.missing.begin(), result.missing.end());
            impl->read = 0;
            if (!result.missing.empty()) return; // Read dependencies, then retry this batch.
            impl->dependencies.clear();
            for (auto& entry : result.entries) impl->entries.push_back(std::move(entry));
            impl->next = end;
            impl->progress.done = end;
        });
}

template class AssetIndex<MaterialKind>;
template class AssetIndex<ModelKind>;
template class AssetIndex<SoundKind>;
template class AssetIndex<ParticleKind>;
template struct AssetIndexSnapshot<ModelKind>;
template struct AssetIndexSnapshot<SoundKind>;
template struct AssetIndexSnapshot<ParticleKind>;
} // namespace apex::assets
