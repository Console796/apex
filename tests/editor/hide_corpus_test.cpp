#include "corpus.hpp"
#include "editor/editor_session.hpp"

#include <set>

using namespace editor_test;

namespace {
using Identity = std::pair<std::string, int>;
struct Inventory {
    std::map<Identity, std::vector<std::string>> blocks;
    std::map<Identity, std::pair<Identity, bool>> locations;
    std::vector<std::string> metadata;
    bool operator==(const Inventory&) const = default;
};
std::string raw(const kv1::Document& doc, const kv1::Node& node) {
    const auto span = node.sourceSpan();
    return std::string(doc.source().substr(span.offset, span.length));
}
// D11 permits sibling/wrapper reordering only. Index every VMF id (including sides),
// retain its owning object and hidden state, and compare all nonstructural subtrees
// byte for byte. Never normalize whitespace, values, duplicate keys or unknown data.
Inventory inventory(const kv1::Document& doc) {
    Inventory result;
    const auto walk = [&](const auto& self, const kv1::Node& node, Identity parent, bool hidden) -> void {
        if (node.kind() != kv1::NodeKind::Block) return;
        hidden |= node.key() == "hidden";
        Identity owner = parent;
        if (const auto* value = node.find("id")) {
            const auto number = value->asInt();
            REQUIRE(number);
            owner = {std::string(node.key()), *number};
            std::vector<std::string> protectedBytes;
            const bool container = node.key() == "world" || node.key() == "entity";
            if (!container) protectedBytes.push_back(raw(doc, node));
            else {
                protectedBytes.emplace_back(node.rawKey());
                for (const auto& child : node.children()) {
                    if (child.kind() == kv1::NodeKind::Block && (child.find("id") || child.key() == "hidden")) continue;
                    protectedBytes.push_back(raw(doc, child));
                }
            }
            REQUIRE(result.blocks.emplace(owner, std::move(protectedBytes)).second);
            result.locations.emplace(owner, std::pair{parent, hidden});
        }
        for (const auto& child : node.children()) self(self, child, owner, hidden);
    };
    walk(walk, doc.root(), {}, false);
    for (const auto& child : doc.root().children())
        if (child.key() != "hidden" && !child.find("id")) result.metadata.push_back(raw(doc, child));
    return result;
}
std::vector<std::filesystem::path> hideMaps() {
    const auto paths = corpusPaths();
    std::vector<std::filesystem::path> selected;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto file = readCorpus(paths[i]);
        const auto hasHidden = [&](const auto& self, const kv1::Node& n) -> bool {
            if (n.kind() == kv1::NodeKind::Block && n.key() == "hidden") return true;
            return std::ranges::any_of(n.children(), [&](const auto& c) { return self(self, c); });
        };
        if (i < 3 || hasHidden(hasHidden, file.kvDocument().root())) selected.push_back(paths[i]);
    }
    return selected;
}
void equivalent(const vmf::VmfFile& original, const Document& doc, const std::filesystem::path& path,
                std::string_view variant) {
    const auto output = doc.write();
    REQUIRE(output);
    const auto parsed = vmf::parse(*output, {original.kvDocument().escapeMode()});
    REQUIRE(parsed);
    const auto before = inventory(original.kvDocument()), after = inventory(parsed->kvDocument());
    CHECK(after.blocks.size() == before.blocks.size());
    CHECK(after.blocks == before.blocks);
    CHECK(after.locations == before.locations);
    CHECK(after.metadata == before.metadata);
    if (const auto* out = std::getenv("APEX_HIDE_OUT"); out && *out) {
        const auto root = std::filesystem::path(std::getenv("APEX_P2CE_DIR")) / "sdk_content/maps";
        const auto destination = std::filesystem::path(out) / variant / path.lexically_relative(root);
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream stream(destination, std::ios::binary);
        REQUIRE(stream.good());
        stream << *output;
        stream.close();
        REQUIRE(stream.good());
    }
}
}

TEST_CASE("[corpus] Hide unhide every visible object preserves original blocks by VMF id") {
    const auto paths = hideMaps();
    if (paths.empty()) { MESSAGE("APEX_P2CE_DIR is unset; skipping hide equivalence corpus"); return; }
    std::size_t exercised = 0;
    for (const auto& path : paths) {
        CAPTURE(path.string());
        const auto file = readCorpus(path);
        Document doc(file);
        EditorSession session;
        session.attach(doc);
        std::vector<NodeId> visible;
        for (const auto h : nodes(doc)) {
            const auto* n = doc.get(h);
            if ((n->block->key() == "entity" || std::holds_alternative<Handle<Solid>>(h)) && isVisible(doc, n->id))
                visible.push_back(n->id);
        }
        REQUIRE(session.setNodeHidden(visible, true));
        for (const auto node : visible) CHECK(session.nodeHidden(node));
        REQUIRE(session.setNodeHidden(visible, false));
        exercised += visible.size();
        equivalent(file, doc, path, "objects");
    }
    MESSAGE("Hide equivalence: " << paths.size() << " maps, " << exercised << " objects");
}

// Known failures reproduced below: recreating wrappers reserializes raw blocks;
// resetting visibility also loses original flag spelling/absence. Fixing source-slot
// preservation requires Document internals, outside this tests-only worktree's scope.
TEST_CASE("[corpus] Originally hidden objects unhide rehide preserve original blocks by VMF id" * doctest::may_fail(true)) {
    const auto paths = hideMaps();
    if (paths.empty()) { MESSAGE("APEX_P2CE_DIR is unset; skipping hidden equivalence corpus"); return; }
    for (const auto& path : paths) {
        CAPTURE(path.string());
        const auto file = readCorpus(path);
        Document doc(file);
        EditorSession session;
        session.attach(doc);
        std::vector<NodeId> hidden;
        for (const auto h : nodes(doc)) {
            const auto* n = doc.get(h);
            if ((n->block->key() == "entity" || std::holds_alternative<Handle<Solid>>(h)) && session.nodeHidden(n->id))
                hidden.push_back(n->id);
        }
        if (hidden.empty()) continue;
        REQUIRE(session.setNodeHidden(hidden, false));
        REQUIRE(session.setNodeHidden(hidden, true));
        equivalent(file, doc, path, "original-hidden");
    }
}

TEST_CASE("[corpus] Visgroup hide unhide preserves original blocks by VMF id" * doctest::may_fail(true)) {
    const auto paths = hideMaps();
    if (paths.empty()) { MESSAGE("APEX_P2CE_DIR is unset; skipping visgroup equivalence corpus"); return; }
    std::size_t toggled = 0;
    for (const auto& path : paths) {
        CAPTURE(path.string());
        const auto file = readCorpus(path);
        Document doc(file);
        EditorSession session;
        session.attach(doc);
        const auto tree = buildVisgroupTree(doc);
        for (const auto& group : tree.nodes) {
            if (tree.members(group.vmfId).empty()) continue;
            const bool shown = tree.shown(group.vmfId);
            REQUIRE(session.setVisgroupShown(group.vmfId, !shown));
            REQUIRE(session.setVisgroupShown(group.vmfId, shown));
            ++toggled;
        }
        equivalent(file, doc, path, "visgroups");
    }
    REQUIRE(toggled > 0);
    MESSAGE("Visgroup equivalence: " << paths.size() << " maps, " << toggled << " groups");
}

TEST_CASE("Hide equivalence regression: original hidden block survives unhide rehide verbatim" * doctest::may_fail(true)) {
    const auto file = parsed("world { id 1 hidden { solid { id 2 side { id 3 material brick } } } }");
    Document doc(file);
    EditorSession session;
    session.attach(doc);
    NodeId solid = 0;
    for (const auto h : nodes(doc)) if (std::holds_alternative<Handle<Solid>>(h)) solid = id(doc, h);
    REQUIRE(solid);
    REQUIRE(session.setNodeHidden({solid}, false));
    REQUIRE(session.setNodeHidden({solid}, true));
    const auto output = vmf::parse(doc.write().value());
    REQUIRE(output);
    CHECK(inventory(output->kvDocument()) == inventory(file.kvDocument()));
}

TEST_CASE("Hide equivalence regression: visgroup cycle preserves absent shown flag" * doctest::may_fail(true)) {
    const auto file = parsed(R"(visgroups { visgroup { name test visgroupid 10 } }
        world { id 1 solid { id 2 editor { visgroupid 10 } } })");
    Document doc(file);
    EditorSession session;
    session.attach(doc);
    REQUIRE(session.setVisgroupShown(10, false));
    REQUIRE(session.setVisgroupShown(10, true));
    const auto output = vmf::parse(doc.write().value());
    REQUIRE(output);
    CHECK(inventory(output->kvDocument()) == inventory(file.kvDocument()));
}

TEST_CASE("Visgroup propagation does not nest wrappers inside an originally hidden brush solid") {
    Document doc(parsed(R"(visgroups { visgroup { name hidden visgroupid 11 } visgroup { name shown visgroupid 12 } }
        world { id 1 solid { id 4 editor { visgroupid 12 } } }
        hidden { entity { id 2 classname func_brush
            hidden { solid { id 3 editor { visgroupshown 1 } } }
            editor { visgroupid 11 visgroupshown 0 } } })"));
    EditorSession session;
    session.attach(doc);
    REQUIRE(session.setVisgroupShown(12, false));
    REQUIRE(session.setVisgroupShown(12, true));
    std::size_t wrappers = 0;
    for (const auto h : nodes(doc)) {
        const auto* node = doc.get(h);
        if (node->block->key() != "hidden") continue;
        ++wrappers;
        for (const auto child : node->children)
            CHECK(doc.get(*doc.handle(child))->block->key() != "hidden");
    }
    CHECK(wrappers == 2);
}
