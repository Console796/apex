#include "editor/instance_key_types.hpp"

#include <map>
#include <memory>

namespace apex::editor {
namespace {
using document::InstanceKeyType;
InstanceKeyType fromFgdType(std::string_view type) {
    const auto lower = toLowerAscii(type);
    for (const auto* name : {"target_destination", "target_source", "npcclass", "pointentityclass", "filterclass"})
        if (lower == name) return InstanceKeyType::Name;
    if (lower == "target_name_or_class") return InstanceKeyType::NameOrClass;
    if (lower == "vector" || lower == "origin" || lower == "vecline") return InstanceKeyType::Position;
    if (lower == "vec_dir") return InstanceKeyType::Direction;
    if (lower == "angle") return InstanceKeyType::Angles;
    if (lower == "axis") return InstanceKeyType::Axis;
    if (lower == "angle_pitch") return InstanceKeyType::Pitch;
    if (lower == "angle_negative_pitch") return InstanceKeyType::NegativePitch;
    if (lower == "sidelist") return InstanceKeyType::SideList;
    return InstanceKeyType::String;
}
} // namespace

InstanceFgdTypes instanceFgdTypes(const assets::EntityClassTable& classes) {
    // Property scans are linear, and collapse asks per key of every entity: memoize per class.
    auto memo = std::make_shared<std::map<std::pair<std::string, std::string>, InstanceKeyType>>();
    InstanceFgdTypes result;
    result.keyTypes = [&classes, memo](std::string_view classname, std::string_view key) {
        auto cacheKey = std::make_pair(toLowerAscii(classname), toLowerAscii(key));
        if (const auto found = memo->find(cacheKey); found != memo->end()) return found->second;
        auto type = document::defaultInstanceKeyType(classname, key);
        if (const auto* entity = classes.find(classname)) {
            for (const auto& property : entity->properties) {
                if (!ciEquals(property.name, key)) continue;
                type = fromFgdType(property.type);
                if (ciEquals(key, "origin")) type = InstanceKeyType::Position;
                else if (ciEquals(key, "angles")) type = InstanceKeyType::Angles;
                else if (ciEquals(key, "yaw")) type = InstanceKeyType::Yaw;
                break;
            }
        }
        memo->emplace(std::move(cacheKey), type);
        return type;
    };
    result.isClassname = [&classes](std::string_view name) { return classes.find(name) != nullptr; };
    return result;
}

} // namespace apex::editor
