#pragma once

#include "core/entity_class.hpp"
#include "document/entity_transform.hpp"

#include <utility>
#include <vector>

namespace apex::document {

class Document;
std::optional<DVec3> spotlightTarget(const Document& doc, const kv1::Node& block);

// The inspector displays these values for absent keys. New entities must serialize them too:
// the game and compile tools do not read the FGD to supply editor defaults.
std::vector<std::pair<std::string, std::string>> missingEntityDefaults(
    const kv1::Node& block, const assets::EntityClass& cls);
// Helpers show every FGD default, read-only ones included, without changing the VMF. What the compiler
// sees is narrower: missingEntityDefaults.
kv1::Node entityHelperValues(const kv1::Node& block, const assets::EntityClass& cls);
std::string entityDisplayModel(const kv1::Node& block, const assets::EntityClass* cls);
bool hasEntityHelper(const assets::EntityClass& cls, std::string_view name);
EntityPose spotlightPose(const kv1::Node& block, EntityPose pose,
                         std::optional<DVec3> target = {});

} // namespace apex::document
