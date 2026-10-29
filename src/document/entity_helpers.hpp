#pragma once

#include "core/entity_class.hpp"
#include "document/entity_transform.hpp"

namespace apex::document {

class Document;
std::optional<DVec3> spotlightTarget(const Document& doc, const kv1::Node& block);

// Helpers use the same effective defaults as the inspector without changing the VMF.
kv1::Node entityHelperValues(const kv1::Node& block, const assets::EntityClass& cls);
std::string entityDisplayModel(const kv1::Node& block, const assets::EntityClass* cls);
bool hasEntityHelper(const assets::EntityClass& cls, std::string_view name);
EntityPose spotlightPose(const kv1::Node& block, EntityPose pose,
                         std::optional<DVec3> target = {});

} // namespace apex::document
