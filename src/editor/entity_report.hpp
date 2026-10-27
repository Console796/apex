#pragma once

#include "editor/editor_session.hpp"

#include <set>

namespace apex::editor {

struct EntityReportFilter {
    std::string classname, name, keyContains;
    bool searchKeys = true, searchValues = true;
    std::optional<int> visgroup; // VMF visgroup id
    bool insideSelection = false;
    std::optional<bool> brush; // nullopt: both
};
struct EntityReportRow {
    NodeId entity;
    std::string classname, name;
    bool operator==(const EntityReportRow&) const = default;
};
// Class supports '*' and is otherwise exact; name/keyContains are substrings.
// Text comparisons are ASCII case-insensitive. Inside means bounds fully enclosed
// by the expanded selection bounds; an empty selection yields no rows (0.4 D14).
std::vector<EntityReportRow> entityReport(const EditorSession& session, const EntityReportFilter& filter = {});
Selection entityReportSelection(std::span<const EntityReportRow> rows);
std::set<NodeId> entityReportIds(std::span<const EntityReportRow> rows);

} // namespace apex::editor
