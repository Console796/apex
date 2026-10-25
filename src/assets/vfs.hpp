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

// Main-thread owner. SourcePP types never cross this boundary. Earlier mounts win.
class Vfs {
public:
    Vfs();
    ~Vfs();
    Vfs(Vfs&&) noexcept;
    Vfs& operator=(Vfs&&) noexcept;
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    static Result<Vfs> load(const GameProfile& profile, JobPool& jobs, AppResolver apps = {});
    // Builds a replacement loose-file index on a worker and waits for it. Existing index is
    // retained on error. Call from the owner thread, never from one of this pool's jobs.
    Result<void> refresh(JobPool& jobs);
    Result<Blob> open(std::string_view path) const;
    bool exists(std::string_view path) const;
    const AddonList& addons() const;
    std::optional<std::size_t> provider(std::string_view path) const;
    // Immediate files only, sorted canonical virtual paths, duplicates removed. '*' and '?'
    // match filenames case-insensitively; directories themselves are not returned.
    std::vector<std::string> list(std::string_view directory, std::string_view glob = "*") const;
    // Recursive files across all mounts, with list's filename glob and canonical ordering.
    // May run concurrently on workers while Vfs is immutable: no refresh, move or destruction
    // until readers finish. Reads only indices, never the archive objects.
    std::vector<std::string> walk(std::string_view directory, std::string_view glob = "*") const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace apex::assets
