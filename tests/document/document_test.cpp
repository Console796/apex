#include "document/document.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

namespace {
using namespace apex;
using namespace apex::document;

vmf::VmfFile parsed(std::string_view bytes) {
    auto file = vmf::parse(bytes);
    REQUIRE(file);
    return std::move(*file);
}
kv1::Node block(std::string_view bytes) {
    auto file = kv1::parse(std::string(bytes));
    REQUIRE(file);
    REQUIRE(file->root().children().size() == 1);
    return file->root().children()[0];
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::vector<NodeHandle> nodes(const Document& doc) {
    std::vector<NodeHandle> result;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto h = *doc.handle(id);
        result.push_back(h);
        for (const auto child : doc.get(h)->children) self(self, child);
    };
    for (const auto id : doc.roots()) walk(walk, id);
    return result;
}
std::string saved(const Document& doc) {
    auto file = doc.toVmf();
    REQUIRE(file);
    return vmf::write(*file);
}

void randomRoundTrip(const std::string& bytes, std::uint32_t seed, int count) {
    CAPTURE(seed);
    Document doc(parsed(bytes));
    REQUIRE(saved(doc) == bytes);
    std::mt19937 random(seed);
    for (int step = 0; step < count; ++step) {
        CAPTURE(step);
        auto all = nodes(doc);
        auto tx = doc.begin("random operation");
        const auto operation = random() % 4;
        if (all.empty() || operation == 0) {
            std::optional<NodeHandle> parent;
            if (!all.empty() && random() % 2) {
                const auto candidate = all[random() % all.size()];
                if (!std::holds_alternative<Handle<Solid>>(candidate)) parent = candidate;
            }
            tx.create(block(random() % 2 ? "entity { classname info_target }" :
                                          "solid { side { material DEV/GRAY } side { } }"), parent);
        } else if (operation == 1) {
            tx.destroy(all[random() % all.size()]);
        } else if (operation == 2) {
            const auto h = all[random() % all.size()];
            std::optional<NodeHandle> parent;
            if (random() % 2) {
                const auto candidate = all[random() % all.size()];
                bool cycle = false;
                for (auto ancestor = doc.get(candidate)->id; ancestor;
                     ancestor = doc.get(*doc.handle(ancestor))->parent) {
                    if (ancestor == doc.get(h)->id) cycle = true;
                }
                if (!cycle && !std::holds_alternative<Handle<Solid>>(candidate)) parent = candidate;
            }
            tx.reparent(h, parent, random() % 3);
        } else {
            const auto h = all[random() % all.size()];
            if (random() % 3 == 0) {
                tx.setKeyValue(h, "apex_random", {});
            } else {
                tx.setKeyValue(h, "apex_random", std::to_string(random()));
            }
        }
        tx.commit();
        // Exercise serialization while edited, rather than merely restoring saved input.
        if (step % 17 == 0) REQUIRE(doc.toVmf());
    }
    const auto edited = saved(doc);
    const auto undoCount = doc.undoCount();
    while (doc.undo()) {}
    CHECK(saved(doc) == bytes);
    CHECK(doc.dirtyNodes().empty());
    CHECK(doc.redoCount() == undoCount);
    while (doc.redo()) {}
    CHECK(saved(doc) == edited);
    while (doc.undo()) {}
    CHECK(saved(doc) == bytes);
}
} // namespace

TEST_CASE("Document typed arenas preserve hierarchy, IDs and byte spelling") {
    const auto bytes = read(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/everything_lf.vmf");
    Document doc(parsed(bytes));
    CHECK(saved(doc) == bytes);
    CHECK(doc.dirtyNodes().empty());
    bool entity = false, solid = false, group = false, visgroup = false;
    bool camera = false, cordon = false, unknown = false;
    for (const auto h : nodes(doc)) {
        entity |= std::holds_alternative<Handle<Entity>>(h);
        solid |= std::holds_alternative<Handle<Solid>>(h);
        group |= std::holds_alternative<Handle<Group>>(h);
        visgroup |= std::holds_alternative<Handle<Visgroup>>(h);
        camera |= std::holds_alternative<Handle<Camera>>(h);
        cordon |= std::holds_alternative<Handle<Cordon>>(h);
        unknown |= std::holds_alternative<Handle<UnknownBlock>>(h);
        for (const auto child : doc.get(h)->children)
            CHECK(doc.get(*doc.handle(child))->parent == doc.get(h)->id);
    }
    CHECK(entity); CHECK(solid); CHECK(group); CHECK(visgroup);
    CHECK(camera); CHECK(cordon); CHECK(unknown);
}

TEST_CASE("Document SideId index follows solid create destroy undo redo") {
    Document doc(parsed("world { id 1 solid { id 90 side { id 40 } side { id 80 } } }\n"));
    REQUIRE(doc.side(40));
    CHECK(doc.side(40)->faceIndex == 0);
    CHECK(doc.side(80)->faceIndex == 1);
    const auto loaded = doc.side(40)->solid;
    NodeHandle created;
    {
        auto tx = doc.begin("create solid");
        created = tx.create(block("solid { id 2 side { id 1 } side { id 2 } }"),
                            doc.handle(doc.roots()[0]));
        tx.commit();
    }
    const auto createdId = doc.get(created)->id;
    CHECK(*doc.get(created)->block->find("id")->asInt() == 91);
    REQUIRE(doc.side(81)); REQUIRE(doc.side(82));
    CHECK(doc.side(81)->solid == std::get<Handle<Solid>>(created));
    CHECK(doc.side(82)->faceIndex == 1);
    REQUIRE(doc.undo());
    CHECK_FALSE(doc.side(81));
    CHECK(doc.get(created) == nullptr);
    REQUIRE(doc.redo());
    REQUIRE(doc.side(81));
    CHECK(doc.side(81)->solid != std::get<Handle<Solid>>(created));
    CHECK(doc.get(created) == nullptr);
    CHECK(doc.get(*doc.handle(createdId)) != nullptr);
    {
        auto tx = doc.begin("destroy loaded");
        tx.destroy(loaded);
        const auto changes = tx.commit();
        CHECK(changes.destroyed == std::vector<NodeHandle>{loaded});
        CHECK(changes.vanishedSides == std::vector<SideId>{40, 80});
    }
    CHECK_FALSE(doc.side(40));
    REQUIRE(doc.undo());
    REQUIRE(doc.side(40));
    CHECK(doc.side(40)->solid != loaded);
    CHECK(doc.get(loaded) == nullptr);
    REQUIRE(doc.redo());
    CHECK_FALSE(doc.side(40));
}

TEST_CASE("Document ChangeSet batches modifications and reports restored handles") {
    Document doc(parsed("world { id 1 solid { id 2 side { id 3 material old } } }"));
    std::vector<ChangeSet> events;
    const auto observer = doc.observe([&](const ChangeSet& changes) {
        events.push_back(changes);
        REQUIRE_THROWS_AS(doc.begin("observer write"), std::logic_error);
    });
    const auto world = *doc.handle(doc.roots()[0]);
    NodeHandle entity;
    {
        auto tx = doc.begin("create");
        entity = tx.create(block("entity { classname info_target }"), world);
        CHECK(events.empty());
        auto changes = tx.commit();
        CHECK(changes.created == std::vector<NodeHandle>{entity});
        REQUIRE(changes.modified.size() == 1);
        CHECK(changes.modified[0].handle == world);
        CHECK(changes.modified[0].flags == Dirty::Hierarchy);
    }
    REQUIRE(events.size() == 1);
    {
        auto tx = doc.begin("two keys");
        tx.setKeyValue(entity, "targetname", "first");
        tx.setKeyValue(entity, "origin", "1 2 3");
        auto changes = tx.commit();
        CHECK(changes.created.empty()); CHECK(changes.destroyed.empty());
        REQUIRE(changes.modified.size() == 1);
        CHECK(changes.modified[0].handle == entity);
        CHECK(changes.modified[0].flags == Dirty::KeyValues);
    }
    {
        auto tx = doc.begin("reparent");
        tx.reparent(entity, {});
        auto changes = tx.commit();
        CHECK(changes.modified.size() == 2);
        for (const auto& modified : changes.modified) CHECK(modified.flags == Dirty::Hierarchy);
    }
    {
        auto tx = doc.begin("material");
        tx.setSideKeyValue(3, "material", "new");
        auto changes = tx.commit();
        REQUIRE(changes.modified.size() == 1);
        CHECK(changes.modified[0].flags == Dirty::Material);
    }
    REQUIRE(doc.undo());
    CHECK(events.back().modified[0].flags == Dirty::Material);
    REQUIRE(doc.redo());
    CHECK(events.back().modified[0].flags == Dirty::Material);
    doc.unobserve(observer);
    auto tx = doc.begin("empty");
    CHECK(tx.commit().empty());
    CHECK(events.size() == 6);
}

TEST_CASE("Document consecutive keyvalue edits coalesce without losing raw before state") {
    const std::string bytes = "entity { id 8 classname info_target targetname odd\t}\n";
    Document doc(parsed(bytes));
    const auto entity = *doc.handle(doc.roots()[0]);
    for (const auto* text : {"a", "b", "c"}) {
        auto tx = doc.begin("typing");
        tx.setKeyValue(entity, "TARGETNAME", text);
        tx.commit();
    }
    CHECK(doc.undoCount() == 1);
    REQUIRE(doc.undo());
    CHECK(saved(doc) == bytes);
    CHECK(doc.dirtyNodes().empty());
    REQUIRE(doc.redo());
    CHECK(saved(doc).find("\"c\"") != std::string::npos);
    {
        auto tx = doc.begin("different key");
        tx.setKeyValue(entity, "origin", "0 0 0");
        tx.commit();
    }
    CHECK(doc.undoCount() == 2);
}

TEST_CASE("Document rollback is silent and rejects stale handles and hierarchy cycles") {
    const std::string bytes = "world { id 1 group { id 2 } }\n";
    Document doc(parsed(bytes));
    std::size_t notifications = 0;
    doc.observe([&](const ChangeSet&) { ++notifications; });
    const auto world = *doc.handle(doc.roots()[0]);
    const auto group = *doc.handle(doc.get(world)->children[0]);
    NodeHandle created;
    {
        auto tx = doc.begin("cancel");
        REQUIRE_THROWS_AS(doc.begin("nested"), std::logic_error);
        REQUIRE_THROWS_AS(tx.reparent(world, group), std::invalid_argument);
        REQUIRE_THROWS_AS(tx.setKeyValue(world, "ID", "4"), std::invalid_argument);
        tx.setKeyValue(world, "note", "discard");
        created = tx.create(block("solid { side { } }"));
        tx.reparent(group, {});
    }
    CHECK(saved(doc) == bytes);
    CHECK(notifications == 0);
    CHECK(doc.undoCount() == 0);
    CHECK(doc.get(created) == nullptr);
    CHECK_FALSE(doc.side(1));
    {
        auto tx = doc.begin("create again");
        const auto h = tx.create(block("solid { side { } }"));
        CHECK(h != created);
        CHECK(doc.side(2)); // ID allocation is not rewound by cancellation.
        REQUIRE_THROWS_AS(tx.destroy(created), std::invalid_argument);
        tx.commit();
        REQUIRE_THROWS_AS(tx.commit(), std::logic_error);
    }
}

TEST_CASE("Document memory cap evicts oldest snapshots and clears redo on a branch") {
    Document doc(parsed(""));
    auto create = [&] {
        auto tx = doc.begin("new entity");
        tx.create(block("entity { classname info_target }"));
        tx.commit();
    };
    create();
    const auto firstBytes = doc.historyBytes();
    create(); create();
    REQUIRE(doc.undoCount() == 3);
    const auto limit = doc.historyBytes() - firstBytes;
    doc.setUndoCap(limit);
    CHECK(doc.historyBytes() <= limit);
    CHECK(doc.undoCount() == 2);
    REQUIRE(doc.undo()); REQUIRE(doc.undo());
    CHECK_FALSE(doc.undo());
    CHECK(doc.roots().size() == 1);
    REQUIRE(doc.redo());
    create();
    CHECK(doc.redoCount() == 0);
    CHECK_FALSE(doc.redo());
    doc.setUndoCap(0);
    CHECK(doc.historyBytes() == 0);
    CHECK_FALSE(doc.undo());
    CHECK_FALSE(doc.redo());
    create();
    CHECK(doc.undoCount() == 0);
}

TEST_CASE("Document selective serialization preserves all bytes outside an edited solid") {
    const auto bytes = read(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/everything_lf.vmf");
    auto file = parsed(bytes);
    const auto span = file.world->solids[0].node->sourceSpan();
    Document doc(file);
    const auto sideId = *file.world->solids[0].sides[0].id()->asInt();
    auto tx = doc.begin("material");
    tx.setSideKeyValue(sideId, "material", "DEV/CHANGED");
    tx.commit();
    const auto output = saved(doc);
    CHECK(output.substr(0, span.offset) == bytes.substr(0, span.offset));
    const auto suffix = bytes.substr(span.offset + span.length);
    REQUIRE(output.size() >= suffix.size());
    CHECK(output.substr(output.size() - suffix.size()) == suffix);
    REQUIRE(doc.undo());
    CHECK(saved(doc) == bytes);
}

TEST_CASE("Document duplicate side IDs and independent ID spaces are preserved") {
    const std::string bytes =
        "world { id 1 solid { id 4 side { id 10 } side { id 10 } } group { id 70 } }\n"
        "visgroups { visgroup { visgroupid 90 } }\n";
    Document doc(parsed(bytes));
    CHECK(doc.side(10)->faceIndex == 0);
    CHECK(saved(doc) == bytes);
    auto tx = doc.begin("fresh IDs");
    const auto e = tx.create(block("entity {}"));
    const auto g = tx.create(block("group {}"));
    const auto v = tx.create(block("visgroup {}"));
    CHECK(*doc.get(e)->block->find("id")->asInt() == 5);
    CHECK(*doc.get(g)->block->find("id")->asInt() == 71);
    CHECK(*doc.get(v)->block->find("visgroupid")->asInt() == 91);
    tx.commit();
    REQUIRE(doc.undo());
    CHECK(saved(doc) == bytes);
}

TEST_CASE("Document property sequences undo byte-identically on VMF fixtures") {
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(APEX_FIXTURE_DIR) / "vmf")) {
        CAPTURE(entry.path().string());
        const auto bytes = read(entry.path());
        for (std::uint32_t seed : {1U, 42U, 20261020U, 0xdeadbeefU})
            randomRoundTrip(bytes, seed, 160);
    }
}

TEST_CASE("Document optional corpus property sequences undo byte-identically") {
    const auto* root = std::getenv("APEX_P2CE_DIR");
    if (!root || !std::filesystem::is_directory(root)) {
        MESSAGE("APEX_P2CE_DIR is unset or invalid; skipping optional document corpus");
        return;
    }
    std::vector<std::filesystem::path> maps;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") {
            maps.push_back(entry.path());
        }
    }
    std::sort(maps.begin(), maps.end());
    if (maps.empty()) MESSAGE("No loose VMF corpus maps; skipping optional document corpus");
    for (std::size_t i = 0; i < std::min<std::size_t>(3, maps.size()); ++i) {
        CAPTURE(maps[i].string());
        randomRoundTrip(read(maps[i]), static_cast<std::uint32_t>(100 + i), 60);
    }
}

TEST_CASE("Document deleting keys before hierarchy slots preserves children and duplicate keys") {
    const std::string bytes =
        "entity { id 5 targetname first group { id 8 } targetname second\n"
        "solid { id 9 side { id 20 } } editor { visgroupid 6 } }\n";
    Document doc(parsed(bytes));
    const auto entity = *doc.handle(doc.roots()[0]);
    auto tx = doc.begin("remove first duplicate");
    tx.setKeyValue(entity, "targetname", {});
    tx.commit();
    auto output = doc.toVmf();
    REQUIRE(output);
    REQUIRE(output->entities.size() == 1);
    CHECK(output->entities[0].groups.size() == 1);
    CHECK(output->entities[0].solids.size() == 1);
    REQUIRE(output->entities[0].findAll("targetname").size() == 1);
    CHECK(*output->entities[0].find("targetname")->text() == "second");
    REQUIRE(doc.side(20));
    REQUIRE(doc.undo());
    CHECK(saved(doc) == bytes);
    REQUIRE(doc.redo());
    output = doc.toVmf();
    REQUIRE(output);
    CHECK(output->entities[0].groups.size() == 1);
    CHECK(output->entities[0].solids.size() == 1);
    const auto editor = *doc.handle(doc.get(entity)->children.back());
    auto visibility = doc.begin("visibility membership");
    visibility.setKeyValue(editor, "visgroupid", "7");
    auto changes = visibility.commit();
    REQUIRE(changes.modified.size() == 1);
    CHECK(changes.modified[0].flags == Dirty::Visibility);
}

TEST_CASE("Document reparent serialization reflects ordered hierarchy before undo") {
    const std::string bytes =
        "world { id 1 solid { id 2 side { id 3 } } group { id 4 } }\n"
        "entity { id 5 classname func_detail }\n";
    Document doc(parsed(bytes));
    const auto world = *doc.handle(doc.roots()[0]);
    const auto solid = doc.side(3)->solid;
    const auto group = *doc.handle(doc.get(world)->children[1]);
    const auto entity = *doc.handle(doc.roots()[1]);
    {
        auto tx = doc.begin("move solid");
        tx.reparent(solid, entity, 0);
        tx.commit();
    }
    auto output = doc.toVmf();
    REQUIRE(output);
    CHECK(output->world->solids.empty());
    CHECK(output->world->groups.size() == 1);
    CHECK(output->entities[0].solids.size() == 1);
    CHECK(doc.side(3)->solid == solid);
    {
        auto tx = doc.begin("reorder root");
        tx.reparent(group, {}, 0);
        tx.commit();
    }
    output = doc.toVmf();
    REQUIRE(output);
    CHECK(output->groups.size() == 1);
    CHECK(output->world->groups.empty());
    REQUIRE(doc.undo()); REQUIRE(doc.undo());
    CHECK(saved(doc) == bytes);
}

TEST_CASE("Document undo redo break typing coalescence and no-op edits stay silent") {
    Document doc(parsed("entity { id 1 targetname original }"));
    const auto entity = *doc.handle(doc.roots()[0]);
    auto edit = [&](const char* key, const char* value) {
        auto tx = doc.begin("edit");
        tx.setKeyValue(entity, key, value);
        tx.commit();
    };
    edit("targetname", "a");
    edit("origin", "0 0 0");
    REQUIRE(doc.undo());
    edit("targetname", "b");
    CHECK(doc.undoCount() == 2);
    CHECK(doc.redoCount() == 0);
    REQUIRE(doc.undo());
    auto output = doc.toVmf();
    REQUIRE(output);
    CHECK(*output->entities[0].find("targetname")->text() == "a");
    const auto before = doc.historyBytes();
    edit("targetname", "a");
    CHECK(doc.historyBytes() == before);
    auto tx = doc.begin("missing key deletion");
    tx.setKeyValue(entity, "missing", {});
    CHECK(tx.commit().empty());
}

TEST_CASE("Document recursive destroy restores descendants and cap retains contiguous redo") {
    const std::string bytes = "world { id 1 group { id 7 solid { id 4 side { id 5 } } } }";
    Document doc(parsed(bytes));
    const auto world = *doc.handle(doc.roots()[0]);
    const auto group = *doc.handle(doc.get(world)->children[0]);
    const auto solid = doc.side(5)->solid;
    {
        auto tx = doc.begin("recursive destroy");
        tx.destroy(group);
        auto changes = tx.commit();
        CHECK(changes.destroyed.size() == 2);
        CHECK(changes.vanishedSides == std::vector<SideId>{5});
    }
    CHECK_FALSE(doc.side(5));
    CHECK(doc.get(group) == nullptr);
    CHECK(doc.get(solid) == nullptr);
    REQUIRE(doc.undo());
    CHECK(saved(doc) == bytes);
    CHECK(doc.side(5));
    CHECK(doc.get(group) == nullptr);
    CHECK(doc.get(solid) == nullptr);
    REQUIRE(doc.redo());
    REQUIRE(doc.undo());
    doc.setUndoCap(0);
    CHECK(doc.redoCount() == 0);
    CHECK(saved(doc) == bytes);
}

TEST_CASE("Document ID overflow fails without altering the document") {
    const std::string bytes = "entity { id 2147483647 }\n";
    Document doc(parsed(bytes));
    {
        auto tx = doc.begin("overflow");
        REQUIRE_THROWS_AS(tx.create(block("entity {}")), std::overflow_error);
    }
    CHECK(saved(doc) == bytes);
    CHECK(doc.undoCount() == 0);
}

TEST_CASE("Document object IDs ignore the world's separate ID") {
    Document doc(parsed("world { id 2000000000 } entity { id 8 } solid { id 12 }\n"));
    auto tx = doc.begin("object ID");
    const auto entity = tx.create(block("entity {}"));
    const auto solid = tx.create(block("solid { side {} }"));
    CHECK(*doc.get(entity)->block->find("id")->asInt() == 13);
    CHECK(*doc.get(solid)->block->find("id")->asInt() == 14);
    tx.commit();
}
