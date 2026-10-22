#pragma once

#include "core/entity_class.hpp"
#include "core/result.hpp"

#include <filesystem>
#include <span>

namespace apex::assets {

// toolpp loads physical files and their relative @includes. Properties are copied out;
// bases are resolved recursively, first base wins, direct class properties override bases.
// Cyclic inheritance is an error; missing bases remain visible in unresolvedBases.
Result<EntityClassTable> loadEntityClasses(std::span<const std::filesystem::path> files);
Result<EntityClassTable> loadEntityClasses(const std::filesystem::path& file);

} // namespace apex::assets
