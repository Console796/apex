#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <vector>

namespace apex::document { class Document; }

namespace apex::editor {

// APEX_AUTOSAVE_DIR overrides the complete autosave root (also useful for tests).
std::filesystem::path autosaveRoot();
bool processRunning(std::uint64_t pid);

struct Recovery {
    std::filesystem::path lock, source, snapshot;
    std::uint64_t pid = 0;
};
Result<std::vector<Recovery>> findRecoveries(const std::filesystem::path& root = autosaveRoot());
Result<void> discardRecovery(const Recovery& recovery);

// Owned and called by the document's main thread. Only serialized bytes reach the worker.
// Destruction waits for pending IO before removing the session lock.
class Autosave {
public:
    explicit Autosave(std::filesystem::path source,
                      std::filesystem::path root = autosaveRoot());
    ~Autosave();
    Autosave(const Autosave&) = delete;
    Autosave& operator=(const Autosave&) = delete;
    Result<bool> snapshot(const document::Document& document);
    Result<void> wait();
    const std::filesystem::path& lockPath() const { return lock_; }
private:
    std::filesystem::path source_, directory_, lock_;
    std::string token_;
    std::optional<std::uint64_t> state_, pendingState_;
    std::future<Result<void>> pending_;
};

} // namespace apex::editor
