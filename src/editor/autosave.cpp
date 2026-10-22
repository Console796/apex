#include "editor/autosave.hpp"

#include "document/document.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <limits>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace apex::editor {
namespace {
namespace fs = std::filesystem;
std::atomic<unsigned> sequence{0};

std::uint64_t pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::string utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {bytes.begin(), bytes.end()};
}

Error ioError(const std::exception& e) { return {ErrorCode::IoError, e.what()}; }

void atomicWrite(const fs::path& path, const std::string& bytes) {
    auto temp = path;
    temp += ".tmp";
    try {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("autosave: atomic rename failed");
#else
        fs::rename(temp, path);
#endif
    } catch (...) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw;
    }
}

std::string lockText(const fs::path& source, const fs::path& snapshot) {
    std::ostringstream out;
    out << pid() << '\n' << std::quoted(utf8(source)) << '\n'
        << std::quoted(utf8(snapshot.filename())) << '\n';
    return out.str();
}

std::vector<fs::path> snapshots(const fs::path& directory, const std::string& token = {}) {
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto& path = entry.path();
        if (entry.is_regular_file() && path.extension() == ".vmf" &&
            (token.empty() || path.stem().string().ends_with("-" + token))) paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}
} // namespace

std::filesystem::path autosaveRoot() {
    if (const auto* overridePath = std::getenv("APEX_AUTOSAVE_DIR")) return overridePath;
#ifdef _WIN32
    if (const auto* base = std::getenv("LOCALAPPDATA")) return fs::path(base) / "apex/autosave";
#else
    if (const auto* base = std::getenv("XDG_STATE_HOME"); base && *base) return fs::path(base) / "apex/autosave";
    if (const auto* home = std::getenv("HOME")) return fs::path(home) / ".local/state/apex/autosave";
#endif
    throw std::runtime_error("autosave: user state directory is unavailable");
}

bool processRunning(std::uint64_t owner) {
    if (!owner) return false;
#ifdef _WIN32
    if (owner > MAXDWORD) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(owner));
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    DWORD status = 0;
    const bool alive = !GetExitCodeProcess(process, &status) || status == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    if (owner > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) return false;
    return kill(static_cast<pid_t>(owner), 0) == 0 || errno != ESRCH;
#endif
}

Autosave::Autosave(fs::path source, fs::path root) : source_(std::move(source)) {
    if (!source_.empty()) source_ = fs::absolute(source_).lexically_normal();
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    token_ = std::to_string(pid()) + "-" + std::to_string(now) + "-" + std::to_string(sequence++);
    // Stable FNV-1a: std::hash need not be stable between application launches.
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : (source_.empty() ? token_ : utf8(source_))) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream suffix;
    suffix << std::hex << hash;
    auto name = source_.empty() ? std::string("Untitled") : utf8(source_.stem());
    for (auto& c : name) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    directory_ = root / (name + "-" + suffix.str());
    fs::create_directories(directory_);
    lock_ = directory_ / (token_ + ".lock");
    atomicWrite(lock_, lockText(source_, {}));
}

Autosave::~Autosave() {
    (void)wait();
    std::error_code ignored;
    fs::remove(lock_, ignored);
}

Result<void> Autosave::wait() {
    if (!pending_.valid()) return {};
    auto result = pending_.get();
    if (result) state_ = pendingState_;
    pendingState_.reset();
    return result;
}

Result<bool> Autosave::snapshot(const document::Document& document) {
    if (pending_.valid()) {
        if (pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        if (auto done = wait(); !done) return std::unexpected(done.error());
    }
    if (state_ == document.historyState()) return false;
    auto bytes = document.write();
    if (!bytes) return std::unexpected(bytes.error());
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto path = directory_ / (std::to_string(now) + "-" + token_ + ".vmf");
    pendingState_ = document.historyState();
    try {
        pending_ = std::async(std::launch::async, [path, lock = lock_, source = source_, bytes = std::move(*bytes)]() -> Result<void> {
            try {
                atomicWrite(path, bytes);
                atomicWrite(lock, lockText(source, path));
                const auto paths = snapshots(path.parent_path());
                for (std::size_t i = 10; i < paths.size(); ++i) fs::remove(paths[i - 10]);
                return {};
            } catch (const std::exception& e) { return std::unexpected(ioError(e)); }
        });
    } catch (const std::exception& e) {
        pendingState_.reset();
        return std::unexpected(ioError(e));
    }
    return true;
}

Result<std::vector<Recovery>> findRecoveries(const fs::path& root) {
    try {
        std::vector<Recovery> result;
        if (!fs::exists(root)) return result;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".lock") continue;
            Recovery recovery;
            recovery.lock = entry.path();
            std::ifstream in(recovery.lock);
            std::string source, last;
            if (!(in >> recovery.pid >> std::quoted(source) >> std::quoted(last)) || processRunning(recovery.pid)) continue;
            recovery.source = fs::path(std::u8string(source.begin(), source.end()));
            const auto paths = snapshots(recovery.lock.parent_path(), recovery.lock.stem().string());
            if (!paths.empty()) recovery.snapshot = paths.back();
            result.push_back(std::move(recovery));
        }
        return result;
    } catch (const std::exception& e) { return std::unexpected(ioError(e)); }
}

Result<void> discardRecovery(const Recovery& recovery) {
    try {
        if (processRunning(recovery.pid)) return std::unexpected(Error{ErrorCode::InvalidArgument, "autosave owner is still running"});
        for (const auto& path : snapshots(recovery.lock.parent_path(), recovery.lock.stem().string())) fs::remove(path);
        fs::remove(recovery.lock);
        return {};
    } catch (const std::exception& e) { return std::unexpected(ioError(e)); }
}
} // namespace apex::editor
