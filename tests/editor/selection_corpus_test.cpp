#include "fixtures.hpp"
#include "editor/selection_ops.hpp"
#include "core/strings.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

using namespace editor_test;

TEST_CASE("Selection GroupIndex P2CE corpus memberships match naive scan and Objects select all") {
    namespace fs = std::filesystem;
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping Selection GroupIndex corpus comparison");
        return;
    }
    const fs::path root = fs::path(install) / "sdk_content/maps";
    REQUIRE(fs::is_directory(root));
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") paths.push_back(entry.path());
    }
    std::ranges::sort(paths);
    REQUIRE_FALSE(paths.empty());
    std::size_t memberships = 0, groupCount = 0;
    for (const auto& path : paths) {
        CAPTURE(path.string());
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
        auto file = vmf::parse(bytes);
        if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
        REQUIRE(file);
        Document doc(*file);
        const auto all = nodes(doc);
        std::map<int, NodeId> definitions;
        std::map<NodeId, int> membership;
        for (const auto h : all) {
            const auto* n = doc.get(h);
            if (std::holds_alternative<Handle<Group>>(h)) {
                REQUIRE(n->block->find("id"));
                const auto number = n->block->find("id")->asInt();
                REQUIRE(number);
                definitions.try_emplace(*number, n->id);
                ++groupCount;
            }
            // Independent naive scan: managed editor nodes refer to their parent;
            // solids keep editor blocks inline. Do not use GroupIndex's readers.
            if (ciEquals(n->block->key(), "editor")) {
                if (const auto* value = n->block->find("groupid")) {
                    const auto number = value->asInt();
                    REQUIRE(number);
                    CHECK(membership.emplace(n->parent, *number).second);
                    ++memberships;
                }
            }
            if (std::holds_alternative<Handle<Solid>>(h)) {
                for (const auto& child : n->block->children()) {
                    if (!ciEquals(child.key(), "editor")) continue;
                    if (const auto* value = child.find("groupid")) {
                        const auto number = value->asInt();
                        REQUIRE(number);
                        CHECK(membership.emplace(n->id, *number).second);
                        ++memberships;
                    }
                }
            }
        }
        GroupIndex groups(doc);
        CHECK(groups.size() == definitions.size());
        std::map<NodeId, std::vector<NodeId>> expectedMembers;
        for (const auto& [object, vmfGroup] : membership) {
            REQUIRE(definitions.contains(vmfGroup));
            const auto parent = definitions.at(vmfGroup);
            expectedMembers[parent].push_back(object);
            CHECK(groups.parentGroup(object) == parent);
            std::set<NodeId> visited;
            auto outer = parent;
            while (membership.contains(outer)) {
                REQUIRE(visited.insert(outer).second);
                REQUIRE(definitions.contains(membership.at(outer)));
                outer = definitions.at(membership.at(outer));
            }
            CHECK(groups.outermostGroup(object) == outer);
        }
        CHECK(groups.membershipCount() == membership.size());
        for (const auto& [vmfId, group] : definitions) {
            CHECK(groups.group(vmfId) == group);
            CHECK(groups.members(group) == expectedMembers[group]);
        }
        // Visible top-level objects: non-world entities and solids outside entities,
        // walking through wrappers. Brush-entity children contribute only their owner.
        std::set<Target> expected;
        for (const auto h : all) {
            const auto* n = doc.get(h);
            if (!isVisible(doc, n->id)) continue;
            if (!std::holds_alternative<Handle<Entity>>(h) && !std::holds_alternative<Handle<Solid>>(h)) continue;
            const auto* classname = n->block->find("classname");
            if (ciEquals(n->block->key(), "world") ||
                (classname && ciEquals(classname->decodedValue().value_or(""), "worldspawn"))) continue;
            bool inEntity = false;
            for (auto parent = n->parent; parent;) {
                const auto ph = *doc.handle(parent);
                const auto* p = doc.get(ph);
                if (std::holds_alternative<Handle<Entity>>(ph) && !ciEquals(p->block->key(), "world")) inEntity = true;
                parent = p->parent;
            }
            if (!inEntity) expected.insert(Target{n->id});
        }
        PickIndex picks(doc);
        Selection selection(SelectionMode::Objects);
        selectAll(selection, picks, groups);
        CHECK(selection.ordered() == std::vector<Target>{expected.begin(), expected.end()});
        invert(selection, picks, groups);
        CHECK(selection.ordered().empty());
    }
    MESSAGE("Selection corpus: " << paths.size() << " maps, " << groupCount << " groups, " << memberships << " memberships");
}
