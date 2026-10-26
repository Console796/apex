#pragma once

#include "document/document.hpp"

#include <filesystem>
#include <span>

namespace apex::editor {

std::optional<std::filesystem::path> resolveInstanceFile(const std::filesystem::path& map,
    std::string_view file, const std::optional<std::filesystem::path>& extraRoot = {});
std::vector<std::string> proxyNames(const document::Document& doc);
bool looksLikeInstanceFile(const document::Document& doc);

class InstanceTemplates {
public:
    static constexpr std::size_t depthLimit = 8;
    using Template = std::shared_ptr<const document::Document>;
    // Ancestors belong to the caller's traversal; traversal failures do not poison the cache.
    Result<Template> load(const std::filesystem::path& resolved,
                          std::span<const std::filesystem::path> ancestors = {});
    Result<Template> invalidate(const std::filesystem::path& resolved);
    void clear() { cache_.clear(); }

private:
    std::map<std::filesystem::path, Result<Template>> cache_;
};

} // namespace apex::editor
