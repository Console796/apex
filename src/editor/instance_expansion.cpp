#include "editor/instance_expansion.hpp"

#include "core/strings.hpp"
#include "document/entity_transform.hpp"

#include <algorithm>

namespace apex::editor {
namespace {
std::string text(const kv1::Node& node, std::string_view key) {
    const auto* value = node.find(key);
    return value ? value->decodedValue().value_or("") : "";
}
std::filesystem::path canonicalPath(const std::filesystem::path& path) {
    std::error_code ec;
    auto result = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : result;
}
void fixed(kv1::Node& node, const document::InstanceFixup& fixup) {
    if (ciEquals(node.key(), "entity") || ciEquals(node.key(), "solid")) {
        node = document::fixedInstanceKeys(node, fixup);
        return;
    }
    for (auto& child : node.children())
        if (child.kind() == kv1::NodeKind::Block) fixed(child, fixup);
}
std::string signature(const document::InstanceFixup& fixup) {
    // Length prefixes keep arbitrary replacement text unambiguous.
    std::string result = std::to_string(static_cast<int>(fixup.style));
    const auto append = [&](const std::string& value) { result += ':' + std::to_string(value.size()) + ':' + value; };
    append(fixup.name);
    for (const auto& [key, value] : fixup.variables) { append(key); append(value); }
    return result;
}
}

void InstanceExpansion::update(const document::Document& document, const std::filesystem::path& map,
                                const std::optional<std::filesystem::path>& extraRoot) {
    contents_.clear();
    problems_.clear();
    dependencies_.clear();
    std::map<Key, std::shared_ptr<const document::Document>> used;
    std::vector<std::filesystem::path> ancestors;
    unsigned autoName = 1;
    const auto expand = [&](const auto& self, const kv1::Node& instance, document::NodeId owner,
                            const std::filesystem::path& referring, const DMat4& parent) -> void {
        const auto file = text(instance, "file");
        const auto path = resolveInstanceFile(referring, file, extraRoot);
        if (!path) {
            problems_.push_back({InstanceProblem::Kind::Unresolved, owner, {}, file, "unresolved instance: " + file});
            return;
        }
        dependencies_[*path].insert(owner);
        if (std::ranges::find(ancestors, *path) != ancestors.end()) {
            problems_.push_back({InstanceProblem::Kind::Cycle, owner, *path, file, "instance cycle: " + file});
            return;
        }
        if (ancestors.size() >= InstanceTemplates::depthLimit) {
            problems_.push_back({InstanceProblem::Kind::DepthLimit, owner, *path, file, "instance depth limit (8): " + file});
            return;
        }
        auto loaded = templates_.load(*path, ancestors);
        if (!loaded) {
            problems_.push_back({InstanceProblem::Kind::Load, owner, *path, file, loaded.error().what()});
            return;
        }
        auto fixup = document::InstanceFixup::parse(instance);
        if (fixup.name.empty()) fixup.name = "InstanceAuto" + std::to_string(autoName++);
        const Key key{*loaded, signature(fixup)};
        auto copy = copies_.find(key);
        std::shared_ptr<const document::Document> keys;
        if (auto found = used.find(key); found != used.end()) keys = found->second;
        else if (copy != copies_.end()) keys = copy->second;
        else {
            auto vmf = (*loaded)->toVmf();
            if (!vmf) {
                problems_.push_back({InstanceProblem::Kind::Load, owner, *path, file, vmf.error().what()});
                return;
            }
            auto kv = vmf->kvDocument();
            fixed(kv.root(), fixup);
            auto bytes = kv1::write(kv);
            auto parsed = bytes ? vmf::parse(*bytes, {kv.escapeMode()}) : Result<vmf::VmfFile>(std::unexpected(bytes.error()));
            if (!parsed) {
                problems_.push_back({InstanceProblem::Kind::Load, owner, *path, file, parsed.error().what()});
                return;
            }
            keys = std::make_shared<const document::Document>(*parsed, 0);
        }
        used.emplace(key, keys);
        const auto transform = parent * document::entityTransform(document::entityPose(instance));
        contents_.push_back({owner, *loaded, keys, transform, fixup, ancestors.size() + 1, *path});
        ancestors.push_back(*path);
        const auto walk = [&](const auto& visit, document::NodeId id) -> void {
            const auto h = *keys->handle(id);
            const auto* node = keys->get(h);
            if (ciEquals(node->block->key(), "hidden")) return;
            if (std::holds_alternative<Handle<document::Entity>>(h) && ciEquals(text(*node->block, "classname"), "func_instance"))
                self(self, *node->block, owner, *path, transform);
            for (auto child : node->children) visit(visit, child);
        };
        for (auto root : keys->roots()) walk(walk, root);
        ancestors.pop_back();
    };
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto h = *document.handle(id);
        const auto* node = document.get(h);
        if (std::holds_alternative<Handle<document::Entity>>(h) && ciEquals(text(*node->block, "classname"), "func_instance"))
            expand(expand, *node->block, id, map, DMat4(1.0));
        for (auto child : node->children) self(self, child);
    };
    for (auto root : document.roots()) walk(walk, root);
    copies_ = std::move(used);
    for (const auto& [path, problem] : reloadProblems_) {
        if (const auto it = dependencies_.find(path); it != dependencies_.end())
            for (auto owner : it->second) { auto p = problem; p.instance = owner; problems_.push_back(std::move(p)); }
    }
}

InstanceReload InstanceExpansion::invalidate(const std::filesystem::path& path) {
    const auto key = canonicalPath(path);
    InstanceReload result;
    if (const auto it = dependencies_.find(key); it != dependencies_.end()) result.users.assign(it->second.begin(), it->second.end());
    if (auto loaded = templates_.invalidate(key); !loaded) {
        result.problem = InstanceProblem{InstanceProblem::Kind::Reload, 0, key, key.filename().string(), loaded.error().what()};
        reloadProblems_.insert_or_assign(key, *result.problem);
    } else reloadProblems_.erase(key);
    return result;
}

} // namespace apex::editor
