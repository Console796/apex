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
struct EntityHelper {
    std::string name, arguments;
};
struct EntityClass {
    std::string name;
    std::string type; // BaseClass, PointClass, SolidClass, etc., without '@'.
    std::vector<std::string> bases;
    std::vector<EntityHelper> helpers;
    std::optional<std::array<int, 3>> color;
    std::optional<std::array<double, 3>> mins;
    std::optional<std::array<double, 3>> maxs;
    std::string iconSprite;
    std::vector<EntityProperty> properties;
    std::vector<EntityIO> inputs, outputs;
};
// Placed as a point: Hammer's point kinds, not only @PointClass. P2CE's FGD declares move_rope and
// keyframe_rope as @MoveClass, NPCs as @NPCClass and filters as @FilterClass. The type keeps the
// file's spelling ("NpcClass"), so the match ignores case.
inline bool isPointClass(const EntityClass& cls) {
    for (const auto kind : {"PointClass", "NPCClass", "KeyFrameClass", "MoveClass", "FilterClass"})
        if (ciEquals(cls.type, kind)) return true;
    return false;
}
// The key a keyframe or animator chain links by: NextKey (ropes) or target (path tracks), when the
// FGD types it target_destination. Empty when the class has neither.
inline std::string chainKey(const EntityClass& cls) {
    for (const auto name : {"nextkey", "target"})
        for (const auto& p : cls.properties)
            if (ciEquals(p.name, name) && ciEquals(p.type, "target_destination")) return p.name;
    return {};
}
struct EntityClassTable {
    std::map<std::string, EntityClass> classes; // lower-case lookup keys
    std::vector<std::string> unresolvedBases; // "class: base", sorted and unique
    const EntityClass* find(std::string_view name) const {
        auto it = classes.find(toLowerAscii(name));
        return it == classes.end() ? nullptr : &it->second;
    }
};
} // namespace apex::assets
