#pragma once

#include "assets/addons.hpp"
#include "core/job_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace apex::assets {

using Blob = std::vector<std::byte>;
// Override Steam discovery for alternate libraries or synthetic installations.
using AppResolver = std::function<std::filesystem::path(std::uint32_t)>;

// A virtual mount served by something other than the filesystem (a plugin). The Vfs indexes
// it once per load/refresh and answers exists/list/walk/walkTask from that index, so only
// open/openPrefix reach the provider again. All calls may arrive from any thread; refresh
// and files are called from the thread that loads or refreshes the Vfs, never concurrently
// with another refresh of the same provider by one Vfs, but older Vfs instances that
// are still alive (a queued walk or thumbnail) may keep calling open. Implementations must
// make that safe and must contain their own failures: an error result means "no files".
class VfsProvider {
public:
    virtual ~VfsProvider() = default;
    virtual std::string name() const = 0;
    // Prepare for a new listing. Optional.
    virtual Result<void> refresh() { return {}; }
    // Every file, as relative virtual paths ('/' separators, any case).
    virtual Result<std::vector<std::string>> files() = 0;
    virtual Result<Blob> open(std::string_view path, std::size_t maxBytes) = 0;
};

// Where a provider sits among the profile mounts (earlier mounts win).
struct VfsPosition {
    enum class Kind { First, Last, Before, After };
    Kind kind = Kind::Last;
    // Case-insensitive substring of a mount's directory or archive path (generic form).
    // An anchor that matches nothing places the provider last and is reported in notes().
    std::string anchor;
};

struct VfsProviderMount {
    std::shared_ptr<VfsProvider> provider;
    VfsPosition position;
};

// Main-thread owner. SourcePP types never cross this boundary. Earlier mounts win.
class Vfs {
public:
    Vfs();
    ~Vfs();
    Vfs(Vfs&&) noexcept;
    Vfs& operator=(Vfs&&) noexcept;
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    // Providers are inserted after the profile and addon mounts are ordered, so a provider can
    // sit between them (VfsPosition); providers at the same position keep the given order.
    static Result<Vfs> load(const GameProfile& profile, JobPool& jobs, AppResolver apps = {},
                            std::vector<VfsProviderMount> providers = {});
    // Builds a replacement loose-file index on a worker and waits for it. Existing index is
    // retained on error. Call from the owner thread, never from one of this pool's jobs.
    Result<void> refresh(JobPool& jobs);
    Result<Blob> open(std::string_view path) const;
    // Read at most maxBytes. Loose files use a bounded read; archive readers may extract
    // the entry first. Useful for cheap header probes without retaining whole payloads.
    Result<Blob> openPrefix(std::string_view path, std::size_t maxBytes) const;
    bool exists(std::string_view path) const;
    const AddonList& addons() const;
    // A compile-only gameinfo with this VFS's ordered mounts and original engine metadata.
    // Its write path stays beside the generated gameinfo, never in the installed game.
    // Provider mounts are left out (the compiler cannot read them); omittedFromCompile() names them
    // so the caller can say so in its log.
    Result<std::string> compileGameInfo(const std::filesystem::path& target = {}) const;
    std::vector<std::string> omittedFromCompile() const;
    // Mount order as display labels (directory, archive or "provider:<name>"), earliest first.
    std::vector<std::string> mountLabels() const;
    // Problems found while placing providers or listing them (anchor not found, files() failed).
    const std::vector<std::string>& notes() const;
    // Absolute path of the loose file that currently wins for `path`, when the winning mount is
    // a directory (not an archive or provider). Used for writes.
    std::optional<std::filesystem::path> loosePath(std::string_view path) const;
    std::optional<std::size_t> provider(std::string_view path) const;
    // Display label of the mount that wins for `path` (as mountLabels), empty when no mount has it.
    std::string mountLabel(std::string_view path) const;
    // Immediate files only, sorted canonical virtual paths, duplicates removed. '*' and '?'
    // match filenames case-insensitively; directories themselves are not returned.
    std::vector<std::string> list(std::string_view directory, std::string_view glob = "*") const;
    // Recursive files across all mounts, with list's filename glob and canonical ordering.
    // May run concurrently on workers while Vfs is immutable: no refresh, move or destruction
    // until readers finish. Reads only indices, never the archive objects.
    // Owns the backing indices, so queued walks survive Vfs destruction. No refresh or
    // mutation until the task finishes; the task never borrows the Vfs wrapper.
    std::function<std::vector<std::string>()> walkTask(
        std::string directory, std::string glob = "*") const;
    // Original spelling of the winning provider, for display.
    std::string displayPath(std::string_view path) const;
    std::vector<std::string> walk(std::string_view directory, std::string_view glob = "*") const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace apex::assets
