#pragma once

#include "document/document.hpp"
#include "document/instance_fixup.hpp"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace apex::document {

// Plan 0.8 D11 rules 2 and 3. Transaction::commit applies them in the same transaction as the
// edit, so undo restores the faces and their lists together; undo and redo themselves never run
// them (the restored lists are already consistent).

// Whether `key` of `classname` holds side IDs: the FGD says sidelist, or it is one of the keys
// VBSP reads as side lists (info_overlay sides, info_overlay_transition sides and sides2,
// env_cubemap sides), which holds without an FGD.
bool isSideListKey(const InstanceKeyTypes& types, std::string_view classname, std::string_view key);

struct SideListEdit {
    NodeHandle entity;
    std::string key;
    std::string value;
};

// Rule 3 first: a face created in this transaction joins every list that held its parent (or an
// earlier ancestor created in the same transaction), when it lies in the parent's plane and, for
// an overlay, when the overlay's quad actually lands on it. Then rule 2: every vanished side is
// removed from every list. Lists are only edited when their text changes.
std::vector<SideListEdit> sideListUpkeep(const Document& doc, const InstanceKeyTypes& types,
                                         std::span<const SideId> vanished,
                                         const std::map<SideId, SideLineage>& lineage);

} // namespace apex::document
