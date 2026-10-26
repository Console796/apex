// Tools-only driver for tools/instance_collapse_crosscheck.py.
#include "document/instance_collapse.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

using namespace apex;
namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}
std::filesystem::path casePath(std::filesystem::path path) {
    path = path.lexically_normal();
    std::filesystem::path result = path.root_path();
    for (const auto& part : path.relative_path()) {
        if (!std::filesystem::is_directory(result)) return {};
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(result)) {
            if (ciEquals(entry.path().filename().string(), part.string())) { result /= entry.path().filename(); found = true; break; }
        }
        if (!found) return {};
    }
    return result;
}
}
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) { std::cerr << "usage: apex_instance_collapse map.vmf [key-types.tsv]\n"; return 2; }
    try {
        auto path = std::filesystem::canonical(argv[1]);
        auto maps = path.parent_path();
        for (auto p = path.parent_path(); p != p.parent_path(); p = p.parent_path())
            if (p.filename() == "maps") { maps = p; break; }
        std::map<std::filesystem::path, std::unique_ptr<document::Document>> templates;
        std::map<const document::Document*, std::filesystem::path> paths;
        document::InstanceCollapseOptions options;
        std::map<std::pair<std::string, std::string>, document::InstanceKeyType> types;
        std::set<std::string> classes;
        if (argc == 3) {
            std::ifstream input(argv[2]);
            std::string line;
            while (std::getline(input, line)) {
                const auto fields = split(line, '\t');
                if (fields.size() != 3) continue;
                classes.insert(std::string(fields[0]));
                types[{std::string(fields[0]), std::string(fields[1])}] = static_cast<document::InstanceKeyType>(std::stoi(std::string(fields[2])));
            }
            options.keyTypes = [&](auto cls, auto key) {
                const auto found = types.find({toLowerAscii(cls), toLowerAscii(key)});
                return found == types.end() ? document::defaultInstanceKeyType(cls, key) : found->second;
            };
            options.isClassname = [&](auto cls) { return classes.contains(toLowerAscii(cls)); };
        }
        document::InstanceTemplateLookup lookup = [&](const document::Document& referring, std::string_view file) -> Result<const document::Document*> {
            std::string spelling(file);
            std::ranges::replace(spelling, '\\', '/');
            auto resolved = casePath(paths.at(&referring).parent_path() / spelling);
            if (resolved.empty()) resolved = casePath(maps / spelling);
            if (resolved.empty()) return std::unexpected(Error{ErrorCode::NotFound, "missing template: " + spelling});
            resolved = std::filesystem::canonical(resolved);
            if (!templates.contains(resolved)) {
                auto parsed = vmf::parse(read(resolved));
                if (!parsed) return std::unexpected(parsed.error());
                auto doc = std::make_unique<document::Document>(*parsed);
                paths[doc.get()] = resolved;
                templates[resolved] = std::move(doc);
            }
            return templates.at(resolved).get();
        };
        auto bytes = read(path);
        for (;;) {
            auto parsed = vmf::parse(bytes);
            if (!parsed) { std::cerr << parsed.error().what() << '\n'; return 1; }
            document::Document parent(*parsed);
            paths[&parent] = path;
            std::optional<document::NodeId> instance;
            const auto find = [&](auto&& self, document::NodeId id) -> void {
                const auto* n = parent.get(*parent.handle(id));
                const auto* cls = n->block->find("classname");
                if (cls && ciEquals(cls->decodedValue().value_or(""), "func_instance") && !instance) instance = id;
                for (auto child : n->children) self(self, child);
            };
            for (auto root : parent.roots()) find(find, root);
            if (!instance) { std::cout << bytes; return 0; }
            const auto original = parent.exportSubtree(*instance);
            const auto* file = original.find("file");
            if (!file) { std::cerr << "instance has no file key\n"; return 1; }
            auto templ = lookup(parent, file->decodedValue().value_or(""));
            if (!templ) { std::cerr << templ.error().what() << '\n'; return 1; }
            auto result = document::collapseInstance(parent, *instance, **templ, lookup, options);
            if (!result) { std::cerr << result.error().what() << '\n'; return 1; }
            options.nextAutoName = result->nextAutoName;
            std::map<document::NodeId, kv1::Node> rewrites;
            for (auto& rewrite : result->connections) rewrites.emplace(rewrite.entity, std::move(rewrite.connections));
            kv1::Document output;
            // Rebuild from live subtrees, identifying edits by document identity, not VMF IDs.
            const auto rebuild = [&](auto&& self, document::NodeId id) -> std::optional<kv1::Node> {
                if (id == *instance) return std::nullopt;
                const auto* n = parent.get(*parent.handle(id));
                auto out = parent.exportSubtree(id);
                // Source child indexes match export order for an unedited freshly parsed map.
                for (auto it = n->sourceChildren.rbegin(); it != n->sourceChildren.rend(); ++it) {
                    auto child = self(self, it->second);
                    if (!child) (void)out.remove(it->first);
                    else out.children()[it->first] = std::move(*child);
                }
                if (rewrites.contains(id)) {
                    if (auto* rows = out.find("connections")) *rows = rewrites.at(id);
                }
                return out;
            };
            for (auto root : parent.roots()) if (auto node = rebuild(rebuild, root)) (void)output.root().append(std::move(*node));
            auto fragment = kv1::parse(result->fragment);
            if (!fragment) { std::cerr << fragment.error().what(); return 1; }
            int nextObject = 1, nextSide = 1, nextGroup = 1, nextVisgroup = 1;
            const auto maxima = [&](auto&& self, const kv1::Node& node) -> void {
                const bool side = ciEquals(node.key(), "side"), group = ciEquals(node.key(), "group");
                if (const auto* id = node.find("id")) {
                    auto& next = side ? nextSide : group ? nextGroup : nextObject;
                    next = std::max(next, id->asInt().value_or(0) + 1);
                }
                if (const auto* id = node.find("visgroupid")) nextVisgroup = std::max(nextVisgroup, id->asInt().value_or(0) + 1);
                for (const auto& child : node.children()) if (child.kind() == kv1::NodeKind::Block) self(self, child);
            };
            maxima(maxima, output.root());
            std::map<int, int> sideIds, groupIds, visgroupIds;
            const auto remap = [&](auto&& self, kv1::Node& node) -> void {
                if (auto* id = node.find("id")) {
                    const int old = id->asInt().value_or(0);
                    int fresh = nextObject++;
                    if (ciEquals(node.key(), "side")) fresh = sideIds[old] = nextSide++;
                    if (ciEquals(node.key(), "group")) {
                        if (!groupIds.contains(old)) groupIds[old] = nextGroup++;
                        fresh = groupIds[old];
                    }
                    (void)id->setValue(std::to_string(fresh));
                }
                for (auto& child : node.children()) {
                    if (child.kind() == kv1::NodeKind::Block) { self(self, child); continue; }
                    const bool group = ciEquals(child.key(), "groupid"), visgroup = ciEquals(child.key(), "visgroupid");
                    if (!group && !visgroup) continue;
                    const int old = child.asInt().value_or(0);
                    auto& mapping = group ? groupIds : visgroupIds;
                    auto& next = group ? nextGroup : nextVisgroup;
                    if (!mapping.contains(old)) mapping[old] = next++;
                    (void)child.setValue(std::to_string(mapping[old]));
                }
            };
            remap(remap, fragment->root());
            for (auto& node : fragment->root().children()) {
                if (!ciEquals(node.key(), "entity") || !options.keyTypes) continue;
                const auto* cls = node.find("classname");
                for (auto& value : node.children()) {
                    if (value.kind() != kv1::NodeKind::Value || options.keyTypes(cls ? cls->decodedValue().value_or("") : "", value.key()) != document::InstanceKeyType::SideList) continue;
                    const auto raw = value.decodedValue().value_or("");
                    std::string mapped;
                    for (auto word : split(raw, ' ')) {
                        const int old = kv1::Node::value("", std::string(word)).asInt().value_or(0);
                        if (!sideIds.contains(old)) continue;
                        if (!mapped.empty()) mapped += ' ';
                        mapped += std::to_string(sideIds[old]);
                    }
                    (void)value.setValue(mapped);
                }
            }
            for (auto node : fragment->root().children()) {
                if (ciEquals(node.key(), "solid") || ciEquals(node.key(), "group")) {
                    auto* world = output.root().find("world");
                    if (world) (void)world->append(std::move(node));
                    else (void)output.root().append(std::move(node));
                } else (void)output.root().append(std::move(node));
            }
            auto written = kv1::write(output);
            if (!written) { std::cerr << written.error().what(); return 1; }
            bytes = std::move(*written);
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
