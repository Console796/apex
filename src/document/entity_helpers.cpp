#include "document/entity_helpers.hpp"

#include "core/entity_helpers.hpp"
#include "document/document.hpp"

#include <cmath>
#include <type_traits>

namespace apex::document {
std::optional<DVec3> spotlightTarget(const Document& doc, const kv1::Node& block) {
    const auto* key = block.find("target");
    const auto name = key ? key->decodedValue().value_or("") : "";
    if (name.empty()) return {};
    std::optional<DVec3> target;
    const auto visit = [&](const auto& self, NodeId id) -> void {
        const auto* node = doc.get(*doc.handle(id));
        if (const auto* value = node->block->find("targetname"); value && !target &&
            ciEquals(value->decodedValue().value_or(""), name)) target = entityPose(*node->block).origin;
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) visit(visit, root);
    return target;
}
std::vector<std::pair<std::string, std::string>> missingEntityDefaults(
    const kv1::Node& block, const assets::EntityClass& cls) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& property : cls.properties) {
        if (property.readOnly || property.defaultValue.empty() || property.name.empty() ||
            ciEquals(property.name, "id") || ciEquals(property.name, "classname") || block.find(property.name)) continue;
        result.emplace_back(property.name, property.defaultValue);
    }
    return result;
}
kv1::Node entityHelperValues(const kv1::Node& block, const assets::EntityClass& cls) {
    auto effective = block;
    for (const auto& p : cls.properties)
        if (!p.defaultValue.empty() && !effective.find(p.name))
            effective.append(kv1::Node::value(p.name, p.defaultValue)).value();
    return effective;
}
bool hasEntityHelper(const assets::EntityClass& cls, std::string_view name) {
    for (const auto& h : cls.helpers) if (ciEquals(h.name, name)) return true;
    return false;
}
std::string entityDisplayModel(const kv1::Node& block, const assets::EntityClass* cls) {
    if (const auto* key = block.find("model")) {
        const auto name = key->decodedValue().value_or("");
        if (!name.empty()) return name.front() == '*' ? std::string{} : name;
    }
    if (cls) for (const auto& h : cls->helpers) {
        auto model = std::visit([](const auto& helper) -> std::string {
            using H = std::decay_t<decltype(helper)>;
            if constexpr (std::is_same_v<H, assets::StudioHelper> || std::is_same_v<H, assets::StudioPropHelper> ||
                          std::is_same_v<H, assets::LightPropHelper>) return helper.model;
            else return {};
        }, assets::parseHelper(h));
        if (!model.empty()) return model;
    }
    return {};
}
EntityPose spotlightPose(const kv1::Node& block, EntityPose pose, std::optional<DVec3> target) {
    // Source light pitch is elevation (positive up), unlike model/entity pitch.
    pose.anglesDeg.x = -pose.anglesDeg.x;
    if (const auto* pitch = block.find("pitch")) {
        const auto value = pitch->asDouble();
        if (value && std::isfinite(*value) && *value != 0) pose.anglesDeg.x = -*value;
    }
    if (target && glm::length(*target - pose.origin) > 1e-6) {
        const auto direction = glm::normalize(*target - pose.origin);
        pose.anglesDeg.x = glm::degrees(std::atan2(-direction.z, glm::length(DVec2{direction.x, direction.y})));
        pose.anglesDeg.y = glm::degrees(std::atan2(direction.y, direction.x));
    }
    const auto axes = angleVectors(pose.anglesDeg);
    pose.basis = {axes.forward, -axes.right, axes.up};
    return pose;
}
} // namespace apex::document
