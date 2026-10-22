#pragma once

#include "editor/group_index.hpp"
#include "editor/selection.hpp"

namespace apex::editor {

struct ExpandedSelection {
    std::vector<NodeId> solidsAndEntities;
    std::vector<SideId> sides;
};

// Owned drawable IDs; groups recurse through membership, brush entities through
// structural children. No render dependency. Stale targets are ignored.
ExpandedSelection expandSelection(const document::Document& document, const GroupIndex& groups,
                                  const Selection& selection);

} // namespace apex::editor
