#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/prefab.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#ifdef _WIN32
#include <process.h>
#define APEX_GETPID _getpid
#else
#include <unistd.h>
#define APEX_GETPID getpid
#endif

using namespace editor_test;

namespace {

using Kv = kv1::Node;
namespace fs = std::filesystem;

std::size_t count(const std::string& text, std::string_view needle) {
    std::size_t n = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}
std::string bytes(const Document& doc) {
    auto text = doc.write();
    REQUIRE(text);
    return *text;
}
std::string valueOf(const Kv* parent, std::string_view key) {
    const auto* node = parent ? parent->find(key) : nullptr;
    const auto v = node ? node->decodedValue() : Result<std::string>(std::unexpected(Error{ErrorCode::NotFound, ""}));
    return v ? *v : std::string();
}

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("apex_prefab_test_" + std::to_string(APEX_GETPID()));
    TempDir() { fs::remove_all(path); fs::create_directories(path); }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

struct Map {
    Document doc{parsed("versioninfo\n{\n\"editorversion\" \"400\"\n}\nworld\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n}\n")};
    EditorSession session;
    NodeHandle world;
    Map() {
        for (const auto r : doc.roots()) {
            if (doc.get(*doc.handle(r))->block->key() == "world") world = *doc.handle(r);
        }
        session.attach(doc);
        session.perform("selection.mode.groups");
    }
    void edit(NodeHandle h, EditorEdit e) {
        auto tx = doc.begin("edit");
        tx.setEditor(h, std::move(e));
        tx.commit();
    }
    int vid(NodeHandle h) { return *vmfId(doc, id(doc, h)); }
};

int visgroupId(const Document& doc, std::string_view name) {
    for (const auto h : nodes(doc)) {
        if (!std::holds_alternative<Handle<Visgroup>>(h)) continue;
        const auto* node = doc.get(h);
        if (valueOf(node->block.get(), "name") == name) return std::stoi(valueOf(node->block.get(), "visgroupid"));
    }
    return -1;
}
std::size_t countOf(const Document& doc, std::string_view classname) {
    std::size_t n = 0;
    for (const auto h : nodes(doc)) {
        const auto* node = doc.get(h);
        if (std::holds_alternative<Handle<Entity>>(h) && node->block && valueOf(node->block.get(), "classname") == classname) ++n;
    }
    return n;
}
// Union of the solids' vertices in the document.
AABB solidBounds(const Document& doc) {
    AABB b;
    for (const auto h : nodes(doc)) {
        if (!std::holds_alternative<Handle<Solid>>(h)) continue;
        const auto g = geometry::solidGeometry(*doc.get(h)->block);
        REQUIRE(g);
        for (const auto v : g->poly.vertices) b.expand(v);
    }
    return b;
}
std::set<std::string> idsOf(const Document& doc) {
    std::set<std::string> out;
    for (const auto h : nodes(doc)) {
        const auto* node = doc.get(h);
        if (!node->block || node->block->key() == "world") continue;
        if (std::holds_alternative<Handle<Solid>>(h) || std::holds_alternative<Handle<Entity>>(h) ||
            std::holds_alternative<Handle<Group>>(h))
            out.insert(std::string(node->block->key()) + valueOf(node->block.get(), "id"));
    }
    return out;
}

// A group {solid in two visgroups, relay with connections}, and a loose box.
struct Source {
    Map m;
    NodeHandle g, box, relay, loose;
    Source() {
        addBlock(m.doc,
                 "visgroups\n{\n visgroup\n {\n \"name\" \"Vis A\"\n \"visgroupid\" \"0\"\n }\n"
                 " visgroup\n {\n \"name\" \"Vis B\"\n \"visgroupid\" \"0\"\n }\n}");
        g = addBlock(m.doc, "group\n{\n\"id\" \"0\"\n}", m.world);
        box = addBox(m.doc, {0, 0, 0}, {64, 64, 64}, m.world);
        m.edit(box, EditorEdit::setGroup(m.vid(g)));
        m.edit(box, EditorEdit::addVisgroup(visgroupId(m.doc, "Vis A")));
        m.edit(box, EditorEdit::addVisgroup(visgroupId(m.doc, "Vis B")));
        relay = addBlock(m.doc, "entity\n{\n\"id\" \"0\"\n\"classname\" \"logic_relay\"\n\"targetname\" \"relay\"\n"
                                "\"origin\" \"128 0 0\"\nconnections\n{\n\"OnTrigger\" \"door,Open,,0,-1\"\n}\n}");
        m.edit(relay, EditorEdit::setGroup(m.vid(g)));
        loose = addBox(m.doc, {512, 0, 0}, {576, 64, 64}, m.world);
    }
    void selectPrefab() {
        m.session.selection().clear();
        m.session.selection().add(id(m.doc, g));
    }
};

} // namespace

TEST_CASE("prefab folders resolve and enumerate recursively with the subfolder as category") {
    TempDir dir;
    CHECK(prefabFolder("/games/p2ce/p2ce", {}) == fs::path("/games/p2ce/hammer/prefabs"));
    CHECK(prefabFolder("/games/p2ce/p2ce", "/mine") == fs::path("/mine"));
    CHECK(listPrefabs(dir.path / "missing").empty());
    fs::create_directories(dir.path / "doors" / "big");
    for (const char* f : {"b.vmf", "a.VMF", "readme.txt", "doors/d.vmf", "doors/big/e.vmf"}) std::ofstream(dir.path / f) << "x";
    const auto list = listPrefabs(dir.path);
    REQUIRE(list.size() == 4);
    CHECK(list[0].category.empty());
    CHECK(list[0].name == "a");
    CHECK(list[1].name == "b");
    CHECK(list[2].category == "doors");
    CHECK(list[2].name == "d");
    CHECK(list[3].category == "doors/big");
}

TEST_CASE("createPrefab writes a standalone map and validates names") {
    Source s;
    TempDir dir;
    CHECK_FALSE(createPrefab(s.m.session, "x", "", dir.path)); // Nothing selected.
    s.selectPrefab();
    CHECK_FALSE(createPrefab(s.m.session, "", "", dir.path));
    CHECK_FALSE(createPrefab(s.m.session, "a/b", "", dir.path));
    CHECK_FALSE(createPrefab(s.m.session, "..", "", dir.path));
    CHECK_FALSE(createPrefab(s.m.session, "ok", "../up", dir.path));
    const auto file = createPrefab(s.m.session, "pair", "props/small", dir.path);
    REQUIRE(file);
    CHECK(*file == dir.path / "props" / "small" / "pair.vmf");
    CHECK_FALSE(createPrefab(s.m.session, "pair", "props/small", dir.path)); // No silent overwrite.
    CHECK(createPrefab(s.m.session, "pair", "props/small", dir.path, true));

    std::ifstream in(*file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    auto map = vmf::parse(text);
    REQUIRE(map);
    // A world holds the solid and group; the entity and the visgroups are top level.
    CHECK(count(text, "worldspawn") == 1);
    CHECK(count(text, "\"prefab\" \"1\"") == 1);
    CHECK(count(text, "door,Open,,0,-1") == 1);
    CHECK(count(text, "\"Vis A\"") == 1);
    const auto kv = kv1::parse(text);
    REQUIRE(kv);
    const Kv* world = kv->root().find("world");
    REQUIRE(world);
    CHECK(world->find("solid"));
    CHECK(world->find("group"));
    CHECK_FALSE(kv->root().find("solid"));
    CHECK(kv->root().find("entity"));
    CHECK(count(text, "\"plane\"") == 6); // Only the group's box; the loose one was not selected.
}

TEST_CASE("placing a prefab keeps group, visgroups and connections with fresh ids, as one undo step") {
    Source s;
    TempDir dir;
    s.selectPrefab();
    const auto file = createPrefab(s.m.session, "pair", "", dir.path);
    REQUIRE(file);

    // Into a fresh map: the bounds' bottom centre lands on the point.
    Map t;
    const auto undo = t.doc.undoCount();
    PrefabPlacement where;
    where.at = {1000, 2000, 32};
    SnapSettings snap;
    snap.suspended = true;
    where.snap = snap;
    REQUIRE(placePrefab(t.session, *file, where));
    CHECK(t.doc.undoCount() == undo + 1);
    const auto after = bytes(t.doc);
    CHECK(count(after, "door,Open,,0,-1") == 1);
    CHECK(visgroupId(t.doc, "Vis A") > 0);
    CHECK(visgroupId(t.doc, "Vis B") > 0);
    // The solid is in both visgroups.
    for (const auto h : nodes(t.doc)) {
        if (!std::holds_alternative<Handle<Solid>>(h)) continue;
        const auto* editor = t.doc.get(h)->block->find("editor");
        REQUIRE(editor);
        CHECK(editor->findAll("visgroupid").size() == 2);
        CHECK(editor->find("groupid"));
    }
    std::size_t groups = 0;
    for (const auto h : nodes(t.doc)) groups += std::holds_alternative<Handle<Group>>(h);
    CHECK(groups == 1);
    // Group membership survived: the solid and relay both name the pasted group.
    std::set<std::string> groupRefs;
    for (const auto h : nodes(t.doc)) {
        const auto* node = t.doc.get(h);
        if (!node->block) continue;
        const auto* editor = node->block->find("editor");
        if (editor && editor->find("groupid")) groupRefs.insert(valueOf(editor, "groupid"));
        if (std::holds_alternative<Handle<Entity>>(h)) {
            for (const auto c : node->children) {
                const auto* cn = t.doc.get(*t.doc.handle(c));
                if (cn->block && cn->block->key() == "editor" && cn->block->find("groupid"))
                    groupRefs.insert(valueOf(cn->block.get(), "groupid"));
            }
        }
    }
    CHECK(groupRefs.size() == 1);
    // Bottom centre of the solid's bounds (the relay is a point, inside the group's span).
    const auto bounds = solidBounds(t.doc);
    CHECK(bounds.min.z == doctest::Approx(32));
    CHECK(bounds.min.x == doctest::Approx(1000 - 64)); // The set spans x 0..128 (the relay's origin), y 0..64.
    CHECK(bounds.min.y == doctest::Approx(2000 - 32));
    REQUIRE(t.doc.undo());
    CHECK(countOf(t.doc, "logic_relay") == 0);

    // Into the source map: nothing collides.
    const auto before = idsOf(s.m.doc);
    const auto sourceUndo = s.m.doc.undoCount();
    REQUIRE(placePrefab(s.m.session, *file, where));
    CHECK(s.m.doc.undoCount() == sourceUndo + 1);
    const auto now = idsOf(s.m.doc);
    CHECK(now.size() == before.size() + 3);
    CHECK(countOf(s.m.doc, "logic_relay") == 2);
    CHECK(count(bytes(s.m.doc), "door,Open,,0,-1") == 2);
    CHECK(visgroupId(s.m.doc, "Vis A") > 0);
    std::size_t sourceGroups = 0;
    for (const auto h : nodes(s.m.doc)) sourceGroups += std::holds_alternative<Handle<Group>>(h);
    CHECK(sourceGroups == 2);
}

TEST_CASE("placement uses the bounds' bottom centre, not the origin") {
    Source s;
    TempDir dir;
    s.selectPrefab();
    const auto file = createPrefab(s.m.session, "pair", "", dir.path);
    REQUIRE(file);
    Map t;
    PrefabPlacement where;
    where.at = {-300, 40, 16};
    SnapSettings snap;
    snap.suspended = true;
    where.snap = snap;
    REQUIRE(placePrefab(t.session, *file, where));
    // The selection's bounds are x 0..128 (solid and relay origin), y 0..64, z 0..64 in the source.
    const auto b = solidBounds(t.doc);
    CHECK(b.min.z == doctest::Approx(16));
    CHECK(b.min.x == doctest::Approx(-300 - 64));
    CHECK(b.min.y == doctest::Approx(40 - 32));
    CHECK(placePrefab(t.session, dir.path / "missing.vmf", where).error().code == ErrorCode::NotFound);
    CHECK_FALSE(placePrefabText(t.session, "not a vmf {", where));
}

TEST_CASE("a prefab's shape and item: local bounds rest the bottom centre on the origin") {
    Source s;
    TempDir dir;
    s.selectPrefab();
    const auto file = createPrefab(s.m.session, "pair", "", dir.path);
    REQUIRE(file);
    const auto shape = prefabShape(*file);
    REQUIRE(shape);
    CHECK(shape->polygons.size() == 6); // The group's box; the entity is a point.
    CHECK(shape->bounds.min == DVec3{0, 0, 0});
    CHECK(shape->bounds.max == DVec3{128, 64, 64}); // Includes the relay's origin.
    const auto item = prefabItem(*file);
    REQUIRE(item.bounds);
    CHECK(item.bounds->min == DVec3{-64, -32, 0});
    CHECK(item.bounds->max == DVec3{64, 32, 64});
    CHECK_FALSE(prefabItem(dir.path / "missing.vmf").bounds);

    Map t;
    REQUIRE(item.place);
    const auto undo = t.doc.undoCount();
    REQUIRE(item.place(t.session, EntityPlacement{{512, 256, 0}, std::nullopt}));
    CHECK(t.doc.undoCount() == undo + 1);
    const auto placed = solidBounds(t.doc);
    CHECK(placed.min.z == doctest::Approx(0.0));
    CHECK(placed.max.x - placed.min.x == doctest::Approx(64.0));
}
