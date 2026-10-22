#pragma once

#include "core/strings.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Plain FGD metadata shared by assets loading and Qt-free editor models.
namespace apex::assets {

struct EntityProperty {
    struct Choice { std::string value, label; };
    struct Flag { std::uint64_t bit; std::string label; bool defaultValue; };
    std::string name, type, displayName, defaultValue, description;
    bool readOnly = false;
    std::vector<Choice> choices;
    std::vector<Flag> flags;
};
struct EntityIO {
    std::string name, type, description;
};
struct EntityClass {
    std::string name;
    std::string type; // BaseClass, PointClass, SolidClass, etc., without '@'.
    std::vector<std::string> bases;
    std::optional<std::array<int, 3>> color;
    std::optional<std::array<double, 3>> mins;
    std::optional<std::array<double, 3>> maxs;
    std::string iconSprite;
    std::vector<EntityProperty> properties;
    std::vector<EntityIO> inputs, outputs;
};
struct EntityClassTable {
    std::map<std::string, EntityClass> classes; // lower-case lookup keys
    std::vector<std::string> unresolvedBases; // "class: base", sorted and unique
    const EntityClass* find(std::string_view name) const {
        auto it = classes.find(toLowerAscii(name));
        return it == classes.end() ? nullptr : &it->second;
    }
};
} // namespace apex::assets
