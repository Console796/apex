#include "assets/vfs.hpp"

#include "assets/path_expand.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <set>
#include <sourcepp/Math.h>
#include <steampp/steampp.h>
#include <vpkpp/format/VPK.h>

namespace apex::assets {
namespace {
namespace fs = std::filesystem;

using detail::expand;
using detail::matches;

Result<std::string> readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + path.string()});
    }
    std::string result(std::istreambuf_iterator<char>{input}, {});
    if (input.bad()) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + path.string()});
    }
    return result;
}

std::string text(const kv1::Node& node) { return node.decodedValue().value_or(""); }
bool flag(const kv1::Node& node, std::string_view key, bool fallback = false) {
    auto child = node.find(key);
    return child ? text(*child) == "1" : fallback;
}

bool virtualPath(std::string_view key) {
    return !key.empty() && key.front() != '/' && key != ".." && !key.starts_with("../") &&
           key.find(':') == std::string_view::npos;
}

bool active(const kv1::Node& node) {
    auto condition = toLowerAscii(node.condition());
    if (condition.empty()) {
        return true;
    }
#ifdef _WIN32
    if (condition == "[$win32]" || condition == "[!$linux]" || condition == "[!$posix]") {
        return true;
    }
    if (condition == "[!$win32]" || condition == "[$linux]" || condition == "[$posix]") {
        return false;
    }
#else
    if (condition == "[$linux]" || condition == "[$posix]" || condition == "[!$win32]") {
        return true;
    }
    if (condition == "[!$linux]" || condition == "[!$posix]" || condition == "[$win32]") {
        return false;
    }
#endif
    return false;
}

} // namespace

struct Vfs::Impl {
    struct Mount {
        fs::path directory;
        std::optional<std::size_t> addon;
        std::unique_ptr<vpkpp::PackFile> archive;
        std::map<std::string, std::string> entries;
    };
    struct LooseFile {
        std::size_t mount;
        fs::path path;
    };
    AddonList addons;
    std::vector<Mount> mounts;
    std::map<std::string, LooseFile> loose;

    Result<void> add(const fs::path& requested, bool required = false,
                     std::optional<std::size_t> addon = {}) {
        auto paths = expand(requested);
        if (paths.empty() && ciEquals(requested.extension().string(), ".vpk") &&
            !toLowerAscii(requested.stem().string()).ends_with("_dir")) {
            paths = expand(requested.parent_path() / (requested.stem().string() + "_dir.vpk"));
        }
        bool mounted = false;
        std::set<std::string> archives;
        for (const auto& path : paths) {
            std::error_code ec;
            if (fs::is_directory(path, ec)) {
                mounts.push_back({path, addon, {}, {}});
                mounted = true;
            } else if (ciEquals(path.extension().string(), ".vpk")) {
                auto archive = vpkpp::VPK::open(path.string());
                if (!archive && !toLowerAscii(path.stem().string()).ends_with("_dir")) {
                    for (const auto& fallback :
                         expand(path.parent_path() / (path.stem().string() + "_dir.vpk"))) {
                        archive = vpkpp::VPK::open(fallback.string());
                        if (archive) {
                            break;
                        }
                    }
                }
                if (!archive) {
                    if (required) {
                        return std::unexpected(
                            Error{ErrorCode::IoError, "Invalid VPK " + path.string()});
                    }
                    continue;
                }
                mounted = true;
                // vpkpp redirects numbered chunks to their directory archive. Wildcards
                // must mount that archive once, rather than once per chunk.
                if (!archives.insert(std::string(archive->getFilepath())).second) {
                    continue;
                }
                Mount mount{{}, addon, std::move(archive), {}};
                mount.archive->runForAllEntries([&](const std::string& entry, const vpkpp::Entry&) {
                    mount.entries.try_emplace(lookupKey(entry), entry);
                });
                mounts.push_back(std::move(mount));
                mounted = true;
            }
        }
        if (required && !mounted) {
            return std::unexpected(
                Error{ErrorCode::NotFound, "Required mount missing: " + requested.string()});
        }
        return {};
    }
};

Vfs::Vfs() : impl_(std::make_unique<Impl>()) {}
Vfs::~Vfs() = default;
Vfs::Vfs(Vfs&&) noexcept = default;
Vfs& Vfs::operator=(Vfs&&) noexcept = default;

Result<Vfs> Vfs::load(const GameProfile& profile, JobPool& jobs, AppResolver apps) try {
    Vfs result;
    auto gameinfos = expand(profile.gameDirectory / "gameinfo.txt");
    if (gameinfos.empty()) {
        return std::unexpected(Error{ErrorCode::NotFound, "gameinfo.txt missing"});
    }
    auto bytes = readText(gameinfos.front());
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto parsed = kv1::parse(std::move(*bytes), profile.escapeMode);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto gameinfo = parsed->root().find("GameInfo");
    auto filesystem = gameinfo ? gameinfo->find("FileSystem") : nullptr;
    auto search = filesystem ? filesystem->find("SearchPaths") : nullptr;
    if (!search) {
        return std::unexpected(Error{ErrorCode::ParseError, "Missing FileSystem/SearchPaths"});
    }
    const auto gameDir = fs::absolute(gameinfos.front().parent_path());
    const auto engineDir = gameDir.parent_path();
    for (const auto& node : search->children()) {
        bool game = false;
        for (auto id : split(node.key(), '+')) {
            game |= ciEquals(id, "game");
        }
        if (!game || !active(node)) {
            continue;
        }
        if (node.kind() != kv1::NodeKind::Value) {
            return std::unexpected(Error{ErrorCode::ParseError, "Search path must be a value"});
        }
        auto path = normalizePath(text(node));
        for (auto [token, replacement] :
             {std::pair{"|gameinfo_path|", gameDir.generic_string() + '/'},
              std::pair{"|all_source_engine_paths|", engineDir.generic_string() + '/'}}) {
            if (toLowerAscii(path).starts_with(token)) {
                path.replace(0, std::char_traits<char>::length(token), replacement);
            }
        }
        auto added = result.impl_->add(engineDir / fs::path(path));
        if (!added) {
            return std::unexpected(added.error());
        }
    }
    auto mountFiles = expand(gameDir / "cfg/mounts.kv");
    if (!mountFiles.empty()) {
        auto mountBytes = readText(mountFiles.front());
        if (!mountBytes) {
            return std::unexpected(mountBytes.error());
        }
        auto mounts = kv1::parse(std::move(*mountBytes), profile.escapeMode);
        if (!mounts) {
            return std::unexpected(mounts.error());
        }
        auto root = mounts->root().find("Mounts");
        if (!root) {
            return std::unexpected(Error{ErrorCode::ParseError, "Missing Mounts block"});
        }
        if (!apps) {
            auto steam = std::make_shared<steampp::Steam>();
            apps = [steam](std::uint32_t id) { return steam->getAppInstallDir(id); };
        }
        // Keep head apps in declaration order, ahead of gameinfo paths.
        std::vector<Impl::Mount> heads;
        for (const auto& app : root->children()) {
            if (!flag(app, "enabled", true) || !active(app)) {
                continue;
            }
            std::uint32_t id = 0;
            auto key = app.key();
            auto number = std::from_chars(key.data(), key.data() + key.size(), id);
            if (number.ec != std::errc{} || number.ptr != key.data() + key.size()) {
                return std::unexpected(Error{ErrorCode::ParseError, "Invalid Steam app ID"});
            }
            const auto install = apps(id);
            const bool required = flag(app, "required");
            if (install.empty()) {
                if (required) {
                    return std::unexpected(Error{ErrorCode::NotFound,
                                                 "Required Steam app missing: " + std::string(key)});
                }
                continue;
            }
            const auto start = result.impl_->mounts.size();
            for (const auto& mod : app.children()) {
                if (mod.kind() != kv1::NodeKind::Block || !active(mod)) {
                    continue;
                }
                const auto base = install / fs::path(mod.key());
                for (const auto& entry : mod.children()) {
                    if (!active(entry)) {
                        continue;
                    }
                    fs::path path = base / fs::path(normalizePath(text(entry)));
                    if (ciEquals(entry.key(), "vpk")) {
                        if (!ciEquals(path.extension().string(), ".vpk")) {
                            path += ".vpk";
                        }
                    } else if (!ciEquals(entry.key(), "dir")) continue;
                    auto added = result.impl_->add(path, required);
                    if (!added) {
                        return std::unexpected(added.error());
                    }
                }
            }
            if (flag(app, "head")) {
                auto& all = result.impl_->mounts;
                for (auto i = start; i < all.size(); ++i) {
                    heads.push_back(std::move(all[i]));
                }
                all.resize(start);
            }
        }
        auto& all = result.impl_->mounts;
        all.insert(all.begin(), std::make_move_iterator(heads.begin()),
                   std::make_move_iterator(heads.end()));
    }
    result.impl_->addons.source = profile.addons.list;
    auto addons = readAddonList(profile);
    if (!addons) result.impl_->addons.problems.push_back(addons.error().what());
    else result.impl_->addons = std::move(*addons);
    auto& impl = *result.impl_;
    const auto baseCount = impl.mounts.size();
    for (std::size_t i = 0; i < impl.addons.entries.size(); ++i) {
        const auto& entry = impl.addons.entries[i];
        if (!entry.mounted || entry.resolved.empty()) continue;
        const bool looseFirst = impl.addons.source == AddonSettings::List::Hammer;
        if (looseFirst) impl.add(entry.resolved, false, i);
        // Multiple VPK priority is unmeasured; retain declaration order.
        for (const auto& vpk : entry.vpks) {
            auto added = impl.add(entry.resolved / vpk, true, i);
            if (!added) impl.addons.problems.push_back(added.error().what());
        }
        if (!looseFirst) impl.add(entry.resolved, false, i);
    }
    std::rotate(impl.mounts.begin(), impl.mounts.begin() + static_cast<std::ptrdiff_t>(baseCount),
                impl.mounts.end());
    auto refreshed = result.refresh(jobs);
    if (!refreshed) {
        return std::unexpected(refreshed.error());
    }
    return result;
} catch (const std::exception& e) {
    return std::unexpected(Error{ErrorCode::IoError, e.what()});
}

Result<void> Vfs::refresh(JobPool& jobs) {
    struct Directory { std::size_t mount; fs::path path; bool addon; };
    std::vector<Directory> directories;
    for (std::size_t i = 0; i < impl_->mounts.size(); ++i)
        if (!impl_->mounts[i].directory.empty()) {
            directories.push_back({i, impl_->mounts[i].directory, impl_->mounts[i].addon.has_value()});
        }
    using Index = decltype(impl_->loose);
    auto promise = std::make_shared<std::promise<Result<Index>>>();
    auto future = promise->get_future();
    jobs.submit(Priority::Normal,
                [directories = std::move(directories), promise](CancelToken token) {
        try {
            Index index;
            for (const auto& [mountIndex, directory, addon] : directories) {
                std::vector<fs::path> files;
                std::error_code ec;
                for (fs::recursive_directory_iterator it(directory, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    if (token.cancelled()) {
                        promise->set_value(std::unexpected(
                            Error{ErrorCode::Internal, "VFS refresh cancelled"}));
                        return;
                    }
                    const auto relative = it->path().lexically_relative(directory);
                    if (addon && ciEquals(relative.begin()->string(), ".assets")) {
                        if (it->is_directory()) it.disable_recursion_pending();
                        continue;
                    }
                    if (it->is_regular_file()) {
                        files.push_back(it->path());
                    }
                }
                if (ec) {
                    promise->set_value(std::unexpected(
                        Error{ErrorCode::IoError,
                              "Cannot index " + directory.string() + ": " + ec.message()}));
                    return;
                }
                std::ranges::sort(files);
                for (const auto& file : files)
                    index.try_emplace(lookupKey(file.lexically_relative(directory).generic_string()),
                                      Impl::LooseFile{mountIndex, file});
            }
            promise->set_value(std::move(index));
        } catch (const std::exception& e) {
            promise->set_value(std::unexpected(Error{ErrorCode::IoError, e.what()}));
        }
    });
    auto index = future.get();
    if (!index) {
        return std::unexpected(index.error());
    }
    impl_->loose = std::move(*index);
    return {};
}

Result<Blob> Vfs::open(std::string_view path) const {
    const auto key = lookupKey(path);
    if (!virtualPath(key)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Invalid virtual path"});
    }
    const auto loose = impl_->loose.find(key);
    for (std::size_t i = 0; i < impl_->mounts.size(); ++i) {
        if (loose != impl_->loose.end() && loose->second.mount == i) {
            auto bytes = readText(loose->second.path);
            if (!bytes) {
                return std::unexpected(bytes.error());
            }
            auto begin = reinterpret_cast<const std::byte*>(bytes->data());
            return Blob(begin, begin + bytes->size());
        }
        const auto& mount = impl_->mounts[i];
        if (auto it = mount.entries.find(key); it != mount.entries.end()) {
            auto bytes = mount.archive->readEntry(it->second);
            if (!bytes) {
                return std::unexpected(Error{ErrorCode::IoError, "Cannot read VPK entry " + key});
            }
            return std::move(*bytes);
        }
    }
    return std::unexpected(Error{ErrorCode::NotFound, "Asset missing: " + key});
}

const AddonList& Vfs::addons() const { return impl_->addons; }

std::optional<std::size_t> Vfs::provider(std::string_view path) const {
    const auto key = lookupKey(path);
    if (!virtualPath(key)) return {};
    const auto loose = impl_->loose.find(key);
    for (std::size_t i = 0; i < impl_->mounts.size(); ++i) {
        const auto& mount = impl_->mounts[i];
        if ((loose != impl_->loose.end() && loose->second.mount == i) || mount.entries.contains(key))
            return mount.addon;
    }
    return {};
}

bool Vfs::exists(std::string_view path) const {
    const auto key = lookupKey(path);
    if (!virtualPath(key)) {
        return false;
    }
    if (impl_->loose.contains(key)) {
        return true;
    }
    return std::ranges::any_of(
        impl_->mounts, [&](const auto& mount) { return mount.entries.contains(key); });
}

std::vector<std::string> Vfs::list(std::string_view directory, std::string_view glob) const {
    auto prefix = lookupKey(directory);
    if (!prefix.empty() && !virtualPath(prefix)) {
        return {};
    }
    if (!prefix.empty()) {
        prefix += '/';
    }
    const auto pattern = toLowerAscii(glob);
    std::set<std::string> paths;
    auto append = [&](const std::string& key) {
        if (!key.starts_with(prefix)) {
            return;
        }
        auto filename = std::string_view(key).substr(prefix.size());
        if (filename.find('/') == std::string_view::npos && matches(pattern, filename)) {
            paths.insert(key);
        }
    };
    for (const auto& [key, file] : impl_->loose) {
        append(key);
    }
    for (const auto& mount : impl_->mounts)
        for (const auto& [key, entry] : mount.entries) {
            append(key);
        }
    return {paths.begin(), paths.end()};
}

std::vector<std::string> Vfs::walk(std::string_view directory, std::string_view glob) const {
    auto prefix = lookupKey(directory);
    if (!prefix.empty() && !virtualPath(prefix)) return {};
    if (!prefix.empty()) prefix += '/';
    const auto pattern = toLowerAscii(glob);
    std::set<std::string> paths;
    const auto append = [&](const auto& entries) {
        for (auto it = entries.lower_bound(prefix);
             it != entries.end() && it->first.starts_with(prefix); ++it) {
            const auto& key = it->first;
            const auto slash = key.rfind('/');
            const auto filename = std::string_view(key).substr(
                slash == std::string::npos ? 0 : slash + 1);
            if (matches(pattern, filename)) paths.insert(key);
        }
    };
    append(impl_->loose);
    for (const auto& mount : impl_->mounts) append(mount.entries);
    return {paths.begin(), paths.end()};
}

std::function<std::vector<std::string>()> Vfs::walkTask(std::string directory, std::string glob) const {
    return [state = impl_, directory = std::move(directory), glob = std::move(glob)] {
        Vfs view;
        view.impl_ = state;
        return view.walk(directory, glob);
    };
}
std::string Vfs::displayPath(std::string_view path) const {
    const auto key = lookupKey(path);
    auto loose = impl_->loose.find(key);
    for (std::size_t i = 0; i < impl_->mounts.size(); ++i) {
        const auto& mount = impl_->mounts[i];
        if (loose != impl_->loose.end() && loose->second.mount == i)
            return loose->second.path.lexically_relative(mount.directory).generic_string();
        if (auto entry = mount.entries.find(key); entry != mount.entries.end()) return entry->second;
    }
    return key;
}

} // namespace apex::assets
