#include "editor/instance_watch_state.hpp"

#include "core/strings.hpp"

namespace apex::editor {
namespace {
namespace fs = std::filesystem;

// The nearest directory above `path` that exists: the place to listen while the file is missing.
fs::path anchor(const fs::path& path) {
    std::error_code ec;
    auto directory = path.parent_path();
    while (!directory.empty() && !fs::is_directory(directory, ec)) {
        const auto next = directory.parent_path();
        if (next == directory) return {};
        directory = next;
    }
    return directory;
}

// The file a missing candidate now exists as, matched case-insensitively like instance resolution.
fs::path appeared(const fs::path& candidate) {
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) return candidate;
    const auto directory = candidate.parent_path();
    if (directory.empty() || !fs::is_directory(directory, ec)) return {};
    const auto name = candidate.filename().string();
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
        if (ciEquals(it->path().filename().string(), name) && it->is_regular_file(ec)) return it->path();
    return {};
}
}

InstanceWatchState::Stamp InstanceWatchState::stampOf(const fs::path& path) {
    std::error_code ec;
    Stamp stamp;
    stamp.exists = fs::is_regular_file(path, ec);
    if (!stamp.exists) return stamp;
    stamp.size = fs::file_size(path, ec);
    stamp.modified = fs::last_write_time(path, ec);
    return stamp;
}

InstanceWatchState::Watches InstanceWatchState::sync(const std::set<fs::path>& dependencies, const std::set<fs::path>& missing) {
    Watches watches;
    std::map<fs::path, Stamp> kept;
    std::map<fs::path, fs::path> anchors;
    const auto track = [&](const fs::path& path) {
        const auto found = stamps_.find(path);
        kept.emplace(path, found != stamps_.end() ? found->second : stampOf(path));
    };
    for (const auto& path : dependencies) {
        track(path);
        watches.files.insert(path);
        watches.directories.insert(path.parent_path());
    }
    for (const auto& path : missing) {
        track(path);
        if (auto where = anchor(path); !where.empty()) {
            watches.directories.insert(where);
            anchors.emplace(path, std::move(where));
        }
    }
    stamps_ = std::move(kept);
    anchors_ = std::move(anchors);
    return watches;
}

std::set<fs::path> InstanceWatchState::directoryChanged(const fs::path& directory) {
    std::set<fs::path> reload;
    for (auto& [path, stamp] : stamps_) {
        const auto anchored = anchors_.find(path);
        const bool isMissing = anchored != anchors_.end();
        if ((isMissing ? anchored->second : path.parent_path()) != directory) continue;
        // A candidate may have appeared under another spelling of its name.
        const auto real = isMissing ? appeared(path) : path;
        const auto now = real.empty() ? Stamp{} : stampOf(real);
        if (now == stamp) continue;
        stamp = now;
        if (!real.empty()) reload.insert(real);
        else if (!isMissing) reload.insert(path);
    }
    return reload;
}

void InstanceWatchState::restamp(const fs::path& path) {
    if (const auto found = stamps_.find(path); found != stamps_.end()) found->second = stampOf(path);
}

}
