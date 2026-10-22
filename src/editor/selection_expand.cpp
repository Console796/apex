#include "editor/selection_expand.hpp"

#include <set>

namespace apex::editor {

ExpandedSelection expandSelection(const document::Document& doc, const GroupIndex& groups,
                                  const Selection& selection) {
    ExpandedSelection result;
    std::set<NodeId> visited;
    const auto expand = [&](const auto& self, NodeId id) -> void {
        if (!visited.insert(id).second) return;
        const auto h = doc.handle(id);
        if (!h) return;
        const auto* node = doc.get(*h);
        if (std::holds_alternative<Handle<document::Group>>(*h)) {
            for (const auto member : groups.members(id)) self(self, member);
        } else if (std::holds_alternative<Handle<document::Solid>>(*h)) {
            result.solidsAndEntities.push_back(id);
        } else {
            bool brush = false;
            const auto solids = [&](const auto& walk, NodeId child) -> void {
                const auto ch = doc.handle(child);
                if (!ch) return;
                if (std::holds_alternative<Handle<document::Solid>>(*ch)) {
                    brush = true;
                    self(self, child);
                } else {
                    for (const auto nested : doc.get(*ch)->children) walk(walk, nested);
                }
            };
            for (const auto child : node->children) solids(solids, child);
            if (std::holds_alternative<Handle<document::Entity>>(*h) && !brush)
                result.solidsAndEntities.push_back(id);
        }
    };
    for (const auto target : selection.ordered()) {
        if (const auto* side = std::get_if<SideId>(&target)) {
            if (doc.side(*side)) result.sides.push_back(*side);
        } else expand(expand, std::get<NodeId>(target));
    }
    return result;
}

} // namespace apex::editor
