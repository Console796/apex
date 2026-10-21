#pragma once

#include "core/result.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::assets {

struct EntityClass {
    std::string name;
    std::string type; // BaseClass, PointClass, SolidClass, etc., without '@'.
    std::vector<std::string> bases;
    std::optional<std::array<int, 3>> color;
    std::optional<std::array<double, 3>> mins;
    std::optional<std::array<double, 3>> maxs;
    std::string iconSprite;
};
struct EntityClassTable {
    std::map<std::string, EntityClass> classes; // lower-case lookup keys
    std::vector<std::string> unresolvedBases; // "class: base", sorted and unique
    const EntityClass* find(std::string_view name) const;
};
// toolpp loads physical files and their relative @includes. Properties are copied out;
// bases are resolved recursively, first base wins, direct class properties override bases.
// Cyclic inheritance is an error; missing bases remain visible in unresolvedBases.
Result<EntityClassTable> loadEntityClasses(std::span<const std::filesystem::path> files);
Result<EntityClassTable> loadEntityClasses(const std::filesystem::path& file);

} // namespace apex::assets
