#pragma once

#include "assets/vfs.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
// An in-memory VfsProvider for tests: path -> contents, with switches for failure modes.
struct MemoryProvider : apex::assets::VfsProvider {
    explicit MemoryProvider(std::string providerName) : label(std::move(providerName)) {}
    std::string name() const override { return label; }
    apex::Result<void> refresh() override {
        ++refreshes;
        if (throwOnRefresh) throw std::runtime_error("refresh boom");
        return {};
    }
    apex::Result<std::vector<std::string>> files() override {
        if (throwOnFiles) throw std::runtime_error("files boom");
        std::lock_guard lock(mutex);
        std::vector<std::string> names;
        for (const auto& [path, bytes] : contents) names.push_back(path);
        return names;
    }
    apex::Result<apex::assets::Blob> open(std::string_view path, std::size_t maxBytes) override {
        ++opens;
        if (throwOnOpen) throw std::runtime_error("open boom");
        std::lock_guard lock(mutex);
        auto it = contents.find(std::string(path));
        if (it == contents.end())
            return std::unexpected(apex::Error{apex::ErrorCode::NotFound, "no such file"});
        apex::assets::Blob blob;
        for (char c : it->second) {
            if (blob.size() >= maxBytes) break;
            blob.push_back(static_cast<std::byte>(c));
        }
        return blob;
    }
    std::string label;
    std::mutex mutex;
    std::map<std::string, std::string> contents;
    std::atomic<int> refreshes{0}, opens{0};
    bool throwOnRefresh = false, throwOnFiles = false, throwOnOpen = false;
};

inline std::string asText(const apex::assets::Blob& blob) {
    return {reinterpret_cast<const char*>(blob.data()), blob.size()};
}
} // namespace
