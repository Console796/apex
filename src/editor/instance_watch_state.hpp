#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <vector>

namespace apex::editor {

// Decides which instance templates a directory event actually touched. A directory notification
// says only that something in the folder changed, so each watched file keeps the stamp it had when
// it was last loaded and only a differing stamp counts. Files that are currently missing are
// tracked at the spot they would have to reappear.
class InstanceWatchState {
public:
    struct Stamp {
        bool exists = false;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};
        bool operator==(const Stamp&) const = default;
    };
    struct Watches {
        std::set<std::filesystem::path> files;
        std::set<std::filesystem::path> directories;
    };

    // Replace the tracked set with the loaded dependencies and the missing candidates. New paths are
    // stamped now; paths that stay keep their stamp. Returns what to hand the file system watcher.
    Watches sync(const std::set<std::filesystem::path>& dependencies, const std::set<std::filesystem::path>& missing);
    // The tracked paths in `directory` whose stamp changed since they were stamped, restamped as returned.
    // A missing candidate that now exists (or whose directory now exists) is reported by its real path.
    std::set<std::filesystem::path> directoryChanged(const std::filesystem::path& directory);
    void restamp(const std::filesystem::path& path);

    static Stamp stampOf(const std::filesystem::path& path);

private:
    std::map<std::filesystem::path, Stamp> stamps_;
    std::map<std::filesystem::path, std::filesystem::path> anchors_; // missing candidate -> directory it is heard from
};

} // namespace apex::editor
