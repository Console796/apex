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
void expandBakeInstances(document::Document& doc, const fs::path& map, const std::optional<fs::path>& extraRoot) {
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
        auto tx = doc.begin("Probe instance expansion");
        for (const auto& node : fragment.root().children()) {
            const bool brush = ciEquals(node.key(), "solid") || ciEquals(node.key(), "group");
            (void)tx.create(node, brush ? world : std::nullopt);
        }
        tx.destroy(handle);
        tx.commit();
    }
    std::cout << "instances expanded=" << instances.size() << " (recursive collapse; no FGD table, as render_pose)\n";
}

} // namespace apex::editor
