#pragma once

#include "core/kv1.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace apex::document {

enum class FixupStyle { Prefix, Postfix, None };
enum class InstanceKeyType { String, Name, NameOrClass, Position, Direction, Angles, Axis,
                             Pitch, NegativePitch, Yaw, SideList };
// The editor supplies FGD types without introducing an assets dependency into document.
using InstanceKeyTypes = std::function<InstanceKeyType(std::string_view classname, std::string_view key)>;
InstanceKeyType defaultInstanceKeyType(std::string_view classname, std::string_view key);

struct InstanceFixup {
    std::string name;
    FixupStyle style = FixupStyle::Prefix;
    std::vector<std::pair<std::string, std::string>> variables;

    static InstanceFixup parse(const kv1::Node& instance);
    // Case insensitive, longest known variable first; unknown identifiers become empty.
    // Dollar signs are not escapes: $$var retains its first dollar sign. With an
    // empty set, srctools consumes dollars alone and leaves identifiers intact.
    std::string substitute(std::string_view text) const;
    std::string fixName(std::string_view text) const;
};

// Resolves scalar values and face materials on a copy. Geometry is left in local space.
kv1::Node fixedInstanceKeys(const kv1::Node& entity, const InstanceFixup& fixup,
                            const InstanceKeyTypes& types = {},
                            const std::function<bool(std::string_view)>& isClassname = {});

} // namespace apex::document
