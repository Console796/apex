#include "editor/instance_collapse_command.hpp"

#include "document/instance_collapse.hpp"
#include "editor/clipboard.hpp"
#include "editor/instance_key_types.hpp"
#include "editor/instance_templates.hpp"

#include <map>

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
} // namespace

Result<InstanceCollapseReport> stageInstanceCollapse(EditorSession& session, document::Document& doc,
                                                      std::span<const NodeId> instances, bool all) {
    InstanceCollapseReport report;
    if (!session.instanceTemplates_) session.instanceTemplates_ = std::make_unique<InstanceTemplates>();
    const auto fgd = instanceFgdTypes(session.entityClasses_);
    std::map<const document::Document*, std::filesystem::path> paths;
    unsigned nextAutoName = 1;
    bool collapsedAny = false;
    // Each instance is planned against the document as the transaction has left it, so a
    // connection rewrite for a later instance also reaches objects pasted for an earlier one.
    auto staged = session.runEdit(all ? "Collapse all instances" : "Collapse instance",
        all ? EditorSession::AfterEdit::Clear : EditorSession::AfterEdit::Computed,
        [&](document::Transaction& tx) -> Result<EditedObjects> {
        EditedObjects created;
        PasteOptions pasteOptions;
        pasteOptions.inPlace = true;
        pasteOptions.textureLock = true;
        pasteOptions.classes = &session.entityClasses_;
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
            if (!resolved) { report.push_back({id, false, "instance file could not be resolved: " + value(block, "file")}); continue; }
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
            options.keyTypes = fgd.keyTypes;
            options.isClassname = fgd.isClassname;
            options.nextAutoName = nextAutoName;
            auto plan = document::collapseInstance(doc, id, **loaded, lookup, options);
            if (!plan) { report.push_back({id, false, plan.error().message}); continue; }
            nextAutoName = plan->nextAutoName;
            report.push_back({id, true, {}});
            collapsedAny = true;

            if (plan->objectCount > 0) {
                auto pasted = stagePaste(doc, tx, plan->fragment, pasteOptions);
                if (!pasted) return std::unexpected(pasted.error());
                const auto parentGroup = integer(plan->parentMembership, "groupid");
                std::vector<int> parentVisgroups;
                for (const auto* v : plan->parentMembership.findAll("visgroupid"))
                    if (const auto n = v->asInt()) parentVisgroups.push_back(*n);
                for (const auto& item : pasted->items) {
                    const auto h = doc.handle(item.node);
                    if (!h) continue;
                    for (const auto vg : parentVisgroups) tx.setEditor(*h, document::EditorEdit::addVisgroup(vg));
                    // Root groups and ungrouped objects inherit the instance's group.
                    if (!item.group && parentGroup) tx.setEditor(*h, document::EditorEdit::setGroup(parentGroup));
                    if (item.kind != EditedObjects::Item::Kind::Entity) continue;
                    // Paste gave every side a fresh id; side lists (overlays) follow them.
                    const auto exported = doc.exportSubtree(item.node);
                    const auto classname = value(exported, "classname");
                    for (const auto& child : exported.children()) {
                        if (child.kind() != kv1::NodeKind::Value ||
                            fgd.keyTypes(classname, child.key()) != document::InstanceKeyType::SideList) continue;
                        std::string mapped;
                        const std::string listText = child.decodedValue().value_or("");
                        for (const auto word : split(listText, ' ')) {
                            const auto old = kv1::Node::value("", std::string(word)).asInt();
                            const auto found = old ? pasted->sideIds.find(*old) : pasted->sideIds.end();
                            if (found == pasted->sideIds.end()) continue;
                            if (!mapped.empty()) mapped += ' ';
                            mapped += std::to_string(found->second);
                        }
                        tx.setKeyValue(*h, std::string(child.key()), std::move(mapped));
                    }
                }
                created.items.insert(created.items.end(), pasted->items.begin(), pasted->items.end());
                created.groupParents.insert(pasted->groupParents.begin(), pasted->groupParents.end());
            }
            for (const auto& rewrite : plan->connections)
                if (const auto target = doc.handle(rewrite.entity)) tx.setConnections(*target, rewrite.connections);
            if (const auto source = doc.handle(id)) tx.destroy(*source);
        }
        // Nothing collapsed: drop the empty transaction instead of committing an undo step.
        if (!collapsedAny) return std::unexpected(Error{ErrorCode::NotFound, "nothing to collapse"});
        return created;
    });
    if (!staged && collapsedAny) return std::unexpected(staged.error());
    return report;
}
} // namespace apex::editor
