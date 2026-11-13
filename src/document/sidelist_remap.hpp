#pragma once

#include "core/kv1.hpp"
#include "document/instance_fixup.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace apex::document {

// What happens to a listed side the map does not cover. Keep it for copies (Hammer: a face that
// was not copied stays referenced); drop it where it cannot exist (an instance's own faces).
enum class UnmappedSide { Keep, Drop };

// The only place sidelist keys (0.8 D11 rule 1) follow re-id'd sides: rewrites every value of
// `entity` whose key `types` reports as SideList, mapping each space-separated side id through
// `sides`. Returns the changed keys with their new values, so a caller can replay them on a
// transaction.
std::vector<std::pair<std::string, std::string>> remapSideLists(kv1::Node& entity, const InstanceKeyTypes& types,
                                                                const std::map<int, int>& sides,
                                                                UnmappedSide unmapped);

} // namespace apex::document
