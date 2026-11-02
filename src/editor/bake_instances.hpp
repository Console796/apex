#pragma once

#include "document/document.hpp"
#include "document/instance_contents.hpp"
#include <filesystem>

namespace apex::editor {
// Expand only the owned worker/tool snapshot; existing parent node IDs are preserved. The result
// names, for every expanded solid, the placement and template solid/sides it was made from, so
// a viewport that draws the instance's contents itself can find that solid's lightmap.
std::vector<document::InstanceSolidOrigin> expandBakeInstances(document::Document& doc, const std::filesystem::path& map,
                         const std::optional<std::filesystem::path>& extraRoot = {});
} // namespace apex::editor
