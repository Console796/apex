#include "editor/bake_instances.hpp"
#include "editor/instance_templates.hpp"
#include "document/instance_collapse.hpp"
#include "core/strings.hpp"

#include <iostream>
#include <map>
#include <stdexcept>

namespace apex::editor {
namespace fs = std::filesystem;
namespace {
template <typename T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().what());
    return std::move(*result);
}
std::string text(const kv1::Node& node, std::string_view key) {
    const auto* value = node.find(key);
    return value ? value->decodedValue().value_or("") : "";
}
}

// Reuse the editor's resolver and document collapse transform/fixups, with fresh document IDs.
std::vector<document::InstanceSolidOrigin> expandBakeInstances(document::Document& doc, const fs::path& map, const std::optional<fs::path>& extraRoot) {
    InstanceTemplates templates;
    std::map<const document::Document*, fs::path> paths{{&doc, map}};
    const auto lookup = [&](const document::Document& referring, std::string_view file)
        -> Result<const document::Document*> {
        const auto path = resolveInstanceFile(paths.at(&referring), file, extraRoot);
        if (!path) return std::unexpected(Error{ErrorCode::NotFound, "Unresolved instance: " + std::string(file)});
        auto loaded = templates.load(*path);
        if (!loaded) return std::unexpected(loaded.error());
        paths[loaded->get()] = *path;
        return loaded->get();
    };
    std::vector<document::NodeId> instances;
    std::vector<document::InstanceSolidOrigin> origins;
    std::optional<document::NodeHandle> world;
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto handle = *doc.handle(id);
        const auto* node = doc.get(handle);
        if (ciEquals(node->block->key(), "hidden")) return;
        const auto cls = toLowerAscii(text(*node->block, "classname"));
        if (cls == "func_instance") instances.push_back(id);
        if (cls == "worldspawn") world = handle;
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
    document::InstanceCollapseOptions options;
    for (const auto id : instances) {
        const auto handle = *doc.handle(id);
        const auto* templ = take(lookup(doc, text(*doc.get(handle)->block, "file")));
        auto collapsed = take(document::collapseInstance(doc, id, *templ, lookup, options));
        options.nextAutoName = collapsed.nextAutoName;
        auto fragment = take(kv1::parse(collapsed.fragment));
        // The created subtree has the fragment's block structure; sides stay inside their solid.
        const auto record = [&](const auto& self, const kv1::Node& source, document::NodeId created) -> void {
            const auto* node = doc.get(*doc.handle(created));
            if (ciEquals(source.key(), "solid")) {
                const auto found = collapsed.origins.find(source.find("id")->asInt().value_or(0));
                if (found == collapsed.origins.end()) return;
                document::InstanceSolidOrigin origin{{id, found->second.nested}, found->second.templateSolid, created, {}, found->second.transform};
                const auto fragmentSides = source.findAll("side"), sides = node->block->findAll("side");
                for (std::size_t i = 0; i < fragmentSides.size() && i < sides.size(); ++i) {
                    const auto template_ = found->second.sides.find(fragmentSides[i]->find("id")->asInt().value_or(0));
                    if (template_ != found->second.sides.end())
                        origin.sides[template_->second] = static_cast<document::SideId>(sides[i]->find("id")->asInt().value_or(0));
                }
                origins.push_back(std::move(origin));
                return;
            }
            std::size_t child = 0;
            for (const auto& block : source.children()) {
                if (block.kind() != kv1::NodeKind::Block) continue;
                if (child < node->children.size()) self(self, block, node->children[child]);
                ++child;
            }
        };
        auto tx = doc.begin("Probe instance expansion");
        for (const auto& node : fragment.root().children()) {
            const bool brush = ciEquals(node.key(), "solid") || ciEquals(node.key(), "group");
            const auto created = tx.create(node, brush ? world : std::nullopt);
            if (node.kind() == kv1::NodeKind::Block) record(record, node, doc.get(created)->id);
        }
        tx.destroy(handle);
        tx.commit();
    }
    std::cout << "instances expanded=" << instances.size() << " (recursive collapse; no FGD table, as render_pose)\n";
    return origins;
}

} // namespace apex::editor
