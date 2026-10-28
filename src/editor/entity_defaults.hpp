#pragma once

#include "core/entity_class.hpp"
#include "core/kv1.hpp"

#include <utility>

namespace apex::editor {

// The inspector displays these values for absent keys. New entities must serialize them too:
// the game and compile tools do not read the FGD to supply editor defaults.
inline std::vector<std::pair<std::string, std::string>> missingEntityDefaults(
    const kv1::Node& block, const assets::EntityClass& cls) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& property : cls.properties) {
        if (property.readOnly || property.defaultValue.empty() || property.name.empty() ||
            ciEquals(property.name, "id") || ciEquals(property.name, "classname") || block.find(property.name)) continue;
        result.emplace_back(property.name, property.defaultValue);
    }
    return result;
}

} // namespace apex::editor
