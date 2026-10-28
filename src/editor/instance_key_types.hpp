#pragma once

#include "core/entity_class.hpp"
#include "document/instance_fixup.hpp"

namespace apex::editor {

// FGD-driven key types for instance collapse. The table must outlive the returned callbacks.
// A (class, key) pair the FGD declares maps its type; origin, angles and yaw keep their
// compiler-defined types; anything else falls back to defaultInstanceKeyType.
struct InstanceFgdTypes {
    document::InstanceKeyTypes keyTypes;
    std::function<bool(std::string_view)> isClassname;
};
InstanceFgdTypes instanceFgdTypes(const assets::EntityClassTable& classes);

} // namespace apex::editor
