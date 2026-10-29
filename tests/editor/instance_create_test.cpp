#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/compile_snapshot.hpp"
#include "editor/instance_templates.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdlib>

using namespace editor_test;
namespace {
struct InstanceDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("apex-create-instance-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    InstanceDirectory() { std::filesystem::create_directories(path); }
    ~InstanceDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
}
TEST_CASE("create instance selection recentres geometry and collapse restores it with one-step undo") {
    InstanceDirectory dir;
    Document doc(parsed("world { id 1 classname worldspawn }"));
    const auto box = addBox(doc, {100, 200, 300}, {164, 264, 364}, doc.handle(doc.roots().front()));
    const auto relay = addBlock(doc, "entity { id 0 classname logic_relay targetname relay origin \"120 220 320\" }");
    EditorSession session;
    session.attach(doc);
    session.setConnectionContext(dir.path / "map.vmf", dir.path);
    session.selection().add(id(doc, box));
    session.selection().add(id(doc, relay));
    const auto original = *doc.write();
    const auto undo = doc.undoCount();
    const auto bounds = *session.selectionBounds();
    REQUIRE(session.createInstanceFromSelection(dir.path / "instances/new.vmf", "instances/new.vmf"));
    CHECK(doc.undoCount() == undo + 1);
    if (const auto* output = std::getenv("APEX_INSTANCE_OUT"); output && *output)
        std::filesystem::copy_file(dir.path / "instances/new.vmf", output, std::filesystem::copy_options::overwrite_existing);
    const auto created = selectedNodes(session.selection());
    REQUIRE(created.size() == 1);
    CHECK(doc.get(*doc.handle(created[0]))->block->find("classname")->decodedValue().value() == "func_instance");
    CHECK(doc.undo());
    CHECK(*doc.write() == original);
    CHECK(std::filesystem::exists(dir.path / "instances/new.vmf"));
    CHECK(doc.redo());
    auto collapsed = session.collapseInstances(created);
    REQUIRE(collapsed);
    REQUIRE(collapsed->size() == 1);
    CHECK((*collapsed)[0].collapsed);
    const auto after = session.selectionBounds();
    REQUIRE(after);
    checkPoint(after->min, bounds.min);
    checkPoint(after->max, bounds.max);
    bool foundRelay = false;
    for (auto h : nodes(doc)) {
        const auto* node = doc.get(h);
        const auto* name = node->block ? node->block->find("targetname") : nullptr;
        if (name && name->decodedValue().value_or("") == "relay") {
            foundRelay = true;
            const auto origin = node->block->find("origin")->asVec3();
            REQUIRE(origin);
            checkPoint({(*origin)[0], (*origin)[1], (*origin)[2]}, {120, 220, 320});
        }
    }
    CHECK(foundRelay);
}
TEST_CASE("create instance warns about incoming and outgoing boundary connections and refuses overwrite") {
    InstanceDirectory dir;
    Document doc(parsed("world { id 1 classname worldspawn }"));
    const auto a = addBlock(doc, "entity { id 2 classname logic_relay targetname a origin \"0 0 0\" connections { OnTrigger \"b,Trigger,,0,-1\" } }");
    addBlock(doc, "entity { id 3 classname logic_relay targetname b origin \"64 0 0\" connections { OnTrigger \"a,Trigger,,0,-1\" } }");
    EditorSession session; session.attach(doc); session.selection().select(id(doc, a));
    INFO(session.copyText().value_or("copy failed"));
    INFO(doc.write().value_or("write failed"));
    REQUIRE(session.instanceBoundaryConnections().size() == 2);
    const auto original = *doc.write();
    CHECK_FALSE(session.createInstanceFromSelection(dir.path / "new.vmf", "new.vmf"));
    CHECK_FALSE(std::filesystem::exists(dir.path / "new.vmf"));
    std::ofstream(dir.path / "new.vmf") << "do not replace";
    CHECK_FALSE(session.createInstanceFromSelection(dir.path / "new.vmf", "new.vmf", true));
    CHECK(*doc.write() == original);
    REQUIRE(session.createInstanceFromSelection(dir.path / "allowed.vmf", "allowed.vmf", true));
}
TEST_CASE("create instance preserves nested instance references and undo restores source text") {
    InstanceDirectory dir;
    const auto map = dir.path / "dir/map.vmf";
    auto file = dir.path / "dir/instances/new.vmf";
    auto leaf = dir.path / "dir/sub/leaf.vmf";
    auto extra = dir.path;
    std::string expected = "../sub/leaf.vmf";
    bool missing = false, shadow = false;
    SUBCASE("map-relative reference moves with the extracted VMF") {}
    SUBCASE("map-relative reference must not resolve to a different file") { shadow = true; }
    SUBCASE("search-root reference keeps its spelling") {
        leaf = dir.path / "extra/sub/leaf.vmf";
        extra = dir.path / "extra";
        expected = "sub/leaf.vmf";
    }
    SUBCASE("same-directory extraction keeps its spelling") {
        file = dir.path / "dir/new.vmf";
        expected = "sub/leaf.vmf";
    }
    SUBCASE("map-relative reference still finds the same file through the search root") {
        extra = dir.path / "dir";
        expected = "sub/leaf.vmf";
    }
    SUBCASE("unresolved reference keeps its spelling") {
        missing = true;
        expected = "sub/leaf.vmf";
    }
    std::filesystem::create_directories(map.parent_path());
    if (!missing) {
        std::filesystem::create_directories(leaf.parent_path());
        std::ofstream(leaf) << "world { id 1 classname worldspawn }";
    }
    if (shadow) {
        std::filesystem::create_directories(file.parent_path() / "sub");
        std::ofstream(file.parent_path() / "sub/leaf.vmf") << "world { id 1 classname worldspawn }";
    }
    const auto resolved = resolveInstanceFile(map, "sub/leaf.vmf", extra);
    if (missing) CHECK_FALSE(resolved);
    else REQUIRE(resolved == std::filesystem::canonical(leaf));
    Document doc(parsed("world { id 1 classname worldspawn }"));
    const auto nested = addBlock(doc, "entity { id 2 classname func_instance file sub/leaf.vmf origin \"64 32 16\" }");
    const auto outside = addBlock(doc, "entity { id 3 classname func_instance file sub/leaf.vmf origin \"128 32 16\" }");
    EditorSession session;
    session.attach(doc);
    session.setConnectionContext(map, extra);
    session.selection().select(id(doc, nested));
    const auto original = *doc.write();
    REQUIRE(session.createInstanceFromSelection(file, "instances/new.vmf"));
    CHECK(doc.get(outside)->block->find("file")->decodedValue().value() == "sub/leaf.vmf");
    std::ifstream stream(file, std::ios::binary);
    REQUIRE(stream);
    const std::string text{std::istreambuf_iterator<char>(stream), {}};
    auto written = kv1::parse(text);
    REQUIRE(written);
    const auto* entity = written->root().find("entity");
    REQUIRE(entity);
    REQUIRE(entity->find("file"));
    const auto reference = entity->find("file")->decodedValue().value();
    CHECK(reference == expected);
    CHECK(resolveInstanceFile(file, reference, extra) == resolved);
    CHECK(doc.undo());
    CHECK(*doc.write() == original);
}
TEST_CASE("create instance never points a nested reference at itself and refuses one it cannot keep") {
    InstanceDirectory dir;
    const auto extract = [](const std::string& reference) {
        auto doc = std::make_unique<Document>(parsed("world { id 1 classname worldspawn }"));
        const auto nested = addBlock(*doc, "entity { id 2 classname func_instance file " + reference + " origin \"64 32 16\" }");
        return std::pair{std::move(doc), nested};
    };
    SUBCASE("a destination named like the template gets an explicit path back to it") {
        // Before the write, "leaf.vmf" from maps/instances finds maps/leaf.vmf through the maps root;
        // after it, the new file would shadow the template and instance itself.
        std::filesystem::create_directories(dir.path / "maps/instances");
        std::ofstream(dir.path / "maps/leaf.vmf") << "world { id 1 classname worldspawn }";
        auto [doc, nested] = extract("leaf.vmf");
        EditorSession session;
        session.attach(*doc);
        session.setConnectionContext(dir.path / "maps/main.vmf", std::nullopt);
        session.selection().select(id(*doc, nested));
        const auto file = dir.path / "maps/instances/leaf.vmf";
        REQUIRE(session.createInstanceFromSelection(file, "instances/leaf.vmf"));
        std::ifstream stream(file, std::ios::binary);
        auto written = kv1::parse(std::string{std::istreambuf_iterator<char>(stream), {}});
        REQUIRE(written);
        CHECK(written->root().find("entity")->find("file")->decodedValue().value() == "../leaf.vmf");
    }
    SUBCASE("a destination outside every instance root is refused before anything is written") {
        std::filesystem::create_directories(dir.path / "work/sub");
        std::filesystem::create_directories(dir.path / "elsewhere");
        std::ofstream(dir.path / "work/sub/leaf.vmf") << "world { id 1 classname worldspawn }";
        auto [doc, nested] = extract("sub/leaf.vmf");
        EditorSession session;
        session.attach(*doc);
        session.setConnectionContext(dir.path / "work/map.vmf", dir.path / "elsewhere");
        session.selection().select(id(*doc, nested));
        const auto original = *doc->write();
        const auto file = dir.path / "work/instances/new.vmf";
        const auto result = session.createInstanceFromSelection(file, "instances/new.vmf");
        REQUIRE_FALSE(result);
        CHECK(std::string(result.error().what()).find("would not resolve") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(file.parent_path()));
        CHECK(*doc->write() == original);
    }
}
TEST_CASE("addon compile targets preserve parent-relative instance references in a contained snapshot") {
    InstanceDirectory dir;
    std::filesystem::create_directories(dir.path / "source");
    std::filesystem::create_directories(dir.path / "addon/mapsrc/instances");
    std::ofstream(dir.path / "addon/mapsrc/instances/a.vmf") << "world {id 1 classname worldspawn} entity {id 2 classname func_instance file b.vmf}";
    std::ofstream(dir.path / "addon/mapsrc/instances/b.vmf") << "world {id 1 classname worldspawn}";
    Document doc(parsed("world {id 1 classname worldspawn} entity {id 2 classname func_instance file \"../addon/mapsrc/instances/a.vmf\"}"));
    editor::CompileGamePaths game;
    game.gameDirectory = dir.path / "game";
    game.addonDirectory = dir.path / "addon";
    auto snapshot = editor::writeCompileSnapshot(doc, dir.path / "source/map.vmf", dir.path / "cache", game);
    REQUIRE_MESSAGE(snapshot, (snapshot ? "" : snapshot.error().what()));
    CHECK(snapshot->paths.bspDirectory == dir.path / "addon/maps");
    CHECK(snapshot->paths.variables.at("addon") == game.addonDirectory.string());
    CHECK(std::filesystem::exists(snapshot->paths.workDirectory / "__apex_instances/0/a.vmf"));
    CHECK(std::filesystem::exists(snapshot->paths.workDirectory / "__apex_instances/1/b.vmf"));
    CHECK(doc.write()->find("../addon/mapsrc/instances/a.vmf") != std::string::npos);
}
