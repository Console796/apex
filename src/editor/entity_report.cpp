#include "editor/entity_report.hpp"

#include "core/strings.hpp"

#include <algorithm>

namespace apex::editor {
namespace {
bool wildcard(std::string_view text, std::string_view pattern) {
    std::size_t t = 0, p = 0, star = std::string_view::npos, retry = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] != '*' && pattern[p] == text[t]) { ++p; ++t; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = t; }
        else if (star != std::string_view::npos) { p = star + 1; t = ++retry; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
std::string value(const kv1::Node& block, std::string_view key) {
    const auto* field = block.find(key);
    return field ? field->decodedValue().value_or("") : "";
}
}
std::vector<EntityReportRow> entityReport(const EditorSession& session, const EntityReportFilter& filter) {
    std::vector<EntityReportRow> result;
    const auto* doc = session.document();
    if (!doc) return result;
    const auto bounds = filter.insideSelection ? session.selectionBounds() : std::nullopt;
    if (filter.insideSelection && !bounds) return result;
    const auto visgroups = buildVisgroupTree(*doc);
    const auto contains = [](std::string_view text, std::string_view needle) { return toLowerAscii(text).contains(toLowerAscii(needle)); };
    const auto brush = [&](const auto& self, NodeId id) -> bool {
        const auto h = *doc->handle(id);
        if (std::holds_alternative<Handle<document::Solid>>(h)) return true;
        for (auto child : doc->get(h)->children) if (self(self, child)) return true;
        return false;
    };
    const auto enclosed = [&](const auto& self, NodeId id) -> bool {
        if (auto box = session.picks().bounds(id)) return bounds->contains(box->min) && bounds->contains(box->max);
        bool any = false;
        for (auto child : doc->get(*doc->handle(id))->children) {
            if (!brush(brush, child)) continue;
            any = true;
            if (!self(self, child)) return false;
        }
        return any;
    };
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto h = *doc->handle(id);
        const auto* node = doc->get(h);
        if (std::holds_alternative<Handle<document::Entity>>(h) && !ciEquals(node->block->key(), "world")) {
            EntityReportRow row{id, value(*node->block, "classname"), value(*node->block, "targetname")};
            bool match = (filter.classname.empty() || wildcard(toLowerAscii(row.classname), toLowerAscii(filter.classname))) &&
                         contains(row.name, filter.name) && (!filter.brush || brush(brush, id) == *filter.brush);
            if (filter.visgroup) {
                const auto memberships = visgroups.visgroupsOf(id);
                match = match && std::ranges::find(memberships, *filter.visgroup) != memberships.end();
            }
            if (!filter.keyContains.empty()) {
                bool found = false;
                for (const auto& entry : node->block->children()) if (entry.kind() == kv1::NodeKind::Value)
                    found |= (filter.searchKeys && contains(entry.key(), filter.keyContains)) ||
                             (filter.searchValues && contains(entry.decodedValue().value_or(""), filter.keyContains));
                match &= found;
            }
            if (match && (!bounds || enclosed(enclosed, id))) result.push_back(std::move(row));
        }
        for (auto child : node->children) self(self, child);
    };
    for (auto root : doc->roots()) walk(walk, root);
    std::ranges::sort(result, {}, &EntityReportRow::entity);
    return result;
}
Selection entityReportSelection(std::span<const EntityReportRow> rows) {
    Selection result(SelectionMode::Objects);
    for (const auto& row : rows) result.add(row.entity);
    return result;
}
std::set<NodeId> entityReportIds(std::span<const EntityReportRow> rows) {
    std::set<NodeId> result;
    for (const auto& row : rows) result.insert(row.entity);
    return result;
}
} // namespace apex::editor
