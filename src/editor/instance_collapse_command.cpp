#include "editor/instance_collapse_command.hpp"

#include "document/instance_collapse.hpp"
#include "editor/clipboard.hpp"
#include "editor/instance_templates.hpp"

#include <algorithm>
#include <map>
#include <tuple>

namespace apex::editor {
namespace {
std::string value(const kv1::Node& node, std::string_view key) {
    const auto* v = node.find(key);
    return v ? v->decodedValue().value_or("") : "";
}
std::optional<int> integer(const kv1::Node& node, std::string_view key) {
    const auto* v = node.find(key);
    if (!v) return {};
    const auto parsed = v->asInt();
    return parsed ? std::optional(*parsed) : std::nullopt;
}
using ConnectionRow = std::tuple<std::string, std::string, std::string>;
ConnectionRow row(const kv1::Node& node) {
    return {std::string(node.rawKey()), std::string(node.rawValue().value_or("")), std::string(node.condition())};
}
kv1::Node mergeConnectionRewrite(const kv1::Node& original, const kv1::Node& current,
                                 const kv1::Node& planned) {
    std::map<ConnectionRow, int> originalRows, plannedRows;
    for (const auto& child : original.children()) ++originalRows[row(child)];
    for (const auto& child : planned.children()) ++plannedRows[row(child)];
    auto removed = originalRows;
    auto added = plannedRows;
    for (const auto& [entry, count] : originalRows) {
        const int shared = std::min(count, plannedRows[entry]);
        removed[entry] -= shared;
        added[entry] -= shared;
    }
    kv1::Node merged = kv1::Node::block("connections");
    for (const auto& child : current.children()) {
        auto& count = removed[row(child)];
        if (count > 0) { --count; continue; }
        (void)merged.append(child);
    }
    for (const auto& child : planned.children()) {
        auto& count = added[row(child)];
        if (count > 0) { (void)merged.append(child); --count; }
    }
    return merged;
}
} // namespace

Result<InstanceCollapseReport> stageInstanceCollapse(EditorSession& session, document::Document& doc,
                                                      std::span<const NodeId> instances, bool all) {
    struct Planned { NodeId id; document::InstanceCollapseResult result; };
    InstanceCollapseReport report;
    std::vector<Planned> plans;
    if (!session.instanceTemplates_) session.instanceTemplates_ = std::make_unique<InstanceTemplates>();
    std::map<const document::Document*, std::filesystem::path> paths;
    paths.emplace(&doc, session.connectionMapPath_);
    unsigned nextAutoName = 1;
    for (const auto id : instances) {
        const auto handle = doc.handle(id);
        if (!handle || !std::holds_alternative<Handle<document::Entity>>(*handle)) {
            report.push_back({id, false, "selected node is not an entity"});
            continue;
        }
        const auto block = doc.exportSubtree(id);
        if (!ciEquals(value(block, "classname"), "func_instance")) {
            report.push_back({id, false, "entity is not func_instance"});
            continue;
        }
        const auto resolved = resolveInstanceFile(session.connectionMapPath_, value(block, "file"), session.connectionExtraRoot_);
        if (!resolved) { report.push_back({id, false, "instance file could not be resolved"}); continue; }
        auto loaded = session.instanceTemplates_->load(*resolved);
        if (!loaded) { report.push_back({id, false, loaded.error().message}); continue; }
        paths[loaded->get()] = *resolved;
        const document::InstanceTemplateLookup lookup = [&](const document::Document& referring, std::string_view file)
            -> Result<const document::Document*> {
            const auto found = paths.find(&referring);
            if (found == paths.end()) return std::unexpected(Error{ErrorCode::NotFound, "unknown referring instance template"});
            const auto path = resolveInstanceFile(found->second, file, session.connectionExtraRoot_);
            if (!path) return std::unexpected(Error{ErrorCode::NotFound, "nested instance file could not be resolved"});
            auto nested = session.instanceTemplates_->load(*path);
            if (!nested) return std::unexpected(nested.error());
            paths[nested->get()] = *path;
            return nested->get();
        };
        document::InstanceCollapseOptions options;
        options.nextAutoName = nextAutoName;
        auto collapsed = document::collapseInstance(doc, id, **loaded, lookup, options);
        if (!collapsed) { report.push_back({id, false, collapsed.error().message}); continue; }
        nextAutoName = collapsed->nextAutoName;
        plans.push_back({id, std::move(*collapsed)});
        report.push_back({id, true, {}});
    }
    if (plans.empty()) return report;
    auto staged = session.runEdit(all ? "Collapse all instances" : "Collapse instance",
        all ? EditorSession::AfterEdit::Clear : EditorSession::AfterEdit::Computed,
        [&](document::Transaction& tx) -> Result<EditedObjects> {
            EditedObjects created;
            std::map<NodeId, kv1::Node> connectionEdits;
            PasteOptions options;
            options.inPlace = true;
            options.textureLock = true;
            options.classes = &session.entityClasses_;
            for (const auto& plan : plans) {
                auto pasted = stagePaste(doc, tx, plan.result.fragment, options);
                if (!pasted) return std::unexpected(pasted.error());
                auto parentGroup = integer(plan.result.parentMembership, "groupid");
                std::vector<int> parentVisgroups;
                for (const auto* v : plan.result.parentMembership.findAll("visgroupid"))
                    if (const auto n = v->asInt()) parentVisgroups.push_back(*n);
                for (const auto& item : pasted->items) {
                    const auto h = doc.handle(item.node);
                    if (!h) continue;
                    for (const auto vg : parentVisgroups) tx.setEditor(*h, document::EditorEdit::addVisgroup(vg));
                    // Root groups and ungrouped objects inherit the instance's group.
                    if (!item.group && parentGroup) tx.setEditor(*h, document::EditorEdit::setGroup(parentGroup));
                }
                created.items.insert(created.items.end(), pasted->items.begin(), pasted->items.end());
                created.groupParents.insert(pasted->groupParents.begin(), pasted->groupParents.end());
                for (const auto& rewrite : plan.result.connections) {
                    if (!doc.handle(rewrite.entity)) continue;
                    const auto source = doc.exportSubtree(rewrite.entity);
                    kv1::Node original = kv1::Node::block("connections");
                    if (const auto* found = source.find("connections")) original = *found;
                    auto current = connectionEdits.contains(rewrite.entity) ? connectionEdits.at(rewrite.entity) : original;
                    connectionEdits[rewrite.entity] = mergeConnectionRewrite(original, current, rewrite.connections);
                }
                if (const auto source = doc.handle(plan.id)) tx.destroy(*source);
            }
            for (auto& [id, connections] : connectionEdits)
                if (const auto source = doc.handle(id)) tx.setConnections(*source, std::move(connections));
            return created;
        });
    if (!staged) return std::unexpected(staged.error());
    return report;
}
} // namespace apex::editor
