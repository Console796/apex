#pragma once

#include "document/document.hpp"
#include <filesystem>

namespace apex::editor {
// Expand only the owned worker/tool snapshot; existing parent node IDs are preserved.
void expandBakeInstances(document::Document& doc, const std::filesystem::path& map,
                         const std::optional<std::filesystem::path>& extraRoot = {});
} // namespace apex::editor
