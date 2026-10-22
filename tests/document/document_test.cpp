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
        tx.setSelection({seed, static_cast<std::uint64_t>(step)},
                        {seed, static_cast<std::uint64_t>(step + 1)});
        const auto operation = random() % 5;
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
        } else if (operation == 4) {
            const auto h = all[random() % all.size()];
            if (std::holds_alternative<Handle<Solid>>(h) ||
                std::holds_alternative<Handle<Entity>>(h) || std::holds_alternative<Handle<Group>>(h)) {
                const int membership = static_cast<int>(random() % 4);
                switch (random() % 5) {
                case 0: tx.setEditor(h, EditorEdit::setGroup(membership)); break;
                case 1: tx.setEditor(h, EditorEdit::setGroup({})); break;
                case 2: tx.setEditor(h, EditorEdit::addVisgroup(membership)); break;
                case 3: tx.setEditor(h, EditorEdit::removeVisgroup(membership)); break;
                case 4: tx.setEditor(h, EditorEdit::setComments(std::to_string(random()))); break;
                }
            }
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

TEST_CASE("P0 red: adding a second entity visgroup preserves the first membership (D5)") {
    Document doc(parsed(read(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf")));
    std::optional<NodeHandle> editor;
    for (const auto h : nodes(doc)) {
        const auto* node = doc.get(h);
        if (node->block->key() == "editor" && node->block->find("visgroupid") &&
            std::holds_alternative<Handle<Entity>>(*doc.handle(node->parent))) editor = h;
    }
    REQUIRE(editor);
    REQUIRE(doc.get(*editor)->block->findAll("visgroupid").size() == 1);
    REQUIRE(*doc.get(*editor)->block->find("visgroupid")->decodedValue() == "1");
    auto tx = doc.begin("add second visgroup");
    tx.setEditor(*doc.handle(doc.get(*editor)->parent), EditorEdit::addVisgroup(2));
    tx.commit();
    auto reloaded = parsed(saved(doc));
    REQUIRE(reloaded.entities.size() == 1);
    REQUIRE(reloaded.entities[0].editor);
    std::vector<std::string> memberships;
    for (const auto* value : reloaded.entities[0].editor->findAll("visgroupid")) {
        auto text = value->text();
        REQUIRE(text);
        memberships.push_back(*text);
    }
    const std::vector<std::string> expected{"1", "2"};
    CHECK_MESSAGE(memberships == expected,
                  "D5 bug: adding visgroup 2 overwrites the entity's existing visgroup 1 membership");
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

TEST_CASE("Document selection metadata round trips and clears on history calls and commits") {
    Document doc(parsed("entity { id 1 targetname original }"));
    const auto entity = *doc.handle(doc.roots()[0]);
    CHECK_FALSE(doc.restoredSelection());
    {
        auto tx = doc.begin("selected edit");
        tx.setSelection({99}, {100});
        tx.setKeyValue(entity, "targetname", "changed");
        tx.setSelection({1, 2}, {3, 4, 5}); // Last call wins.
        tx.commit();
        CHECK_THROWS_AS(tx.setSelection({}, {}), std::logic_error);
    }
    REQUIRE(doc.undo());
    CHECK(doc.restoredSelection() == std::optional<SelectionBlob>{{1, 2}});
    CHECK_FALSE(doc.undo());
    CHECK_FALSE(doc.restoredSelection());
    REQUIRE(doc.redo());
    CHECK(doc.restoredSelection() == std::optional<SelectionBlob>{{3, 4, 5}});
    CHECK_FALSE(doc.redo());
    CHECK_FALSE(doc.restoredSelection());
    REQUIRE(doc.undo());
    {
        auto tx = doc.begin("plain edit");
        tx.setKeyValue(entity, "origin", "0 0 0");
        tx.commit();
    }
    CHECK_FALSE(doc.restoredSelection());
    REQUIRE(doc.undo());
    CHECK_FALSE(doc.restoredSelection());
    REQUIRE(doc.redo());
    CHECK_FALSE(doc.restoredSelection());
}

TEST_CASE("Document selection blobs survive repeated undo and redo across entries") {
    Document doc(parsed("entity { id 1 }"));
    const auto entity = *doc.handle(doc.roots()[0]);
    {
        auto tx = doc.begin("name");
        tx.setKeyValue(entity, "targetname", "named");
        tx.setSelection({1}, {2});
        tx.commit();
    }
    {
        auto tx = doc.begin("origin");
        tx.setKeyValue(entity, "origin", "0 0 0");
        tx.setSelection({}, {});
        tx.commit();
    }
    for (int repeat = 0; repeat < 2; ++repeat) {
        REQUIRE(doc.undo());
        CHECK(doc.restoredSelection() == std::optional<SelectionBlob>{SelectionBlob{}});
        REQUIRE(doc.undo());
        CHECK(doc.restoredSelection() == std::optional<SelectionBlob>{{1}});
        REQUIRE(doc.redo());
        CHECK(doc.restoredSelection() == std::optional<SelectionBlob>{{2}});
        REQUIRE(doc.redo());
        CHECK(doc.restoredSelection() == std::optional<SelectionBlob>{SelectionBlob{}});
    }
}

TEST_CASE("Document coalescing keeps first before and latest after selection metadata") {
    for (const auto recorded : {0U, 1U, 2U, 3U, 5U, 7U}) {
        CAPTURE(recorded);
        Document doc(parsed("entity { id 1 targetname original }"));
        const auto entity = *doc.handle(doc.roots()[0]);
        for (unsigned edit = 0; edit < 3; ++edit) {
            auto tx = doc.begin("typing");
            tx.setKeyValue(entity, "targetname", std::to_string(edit));
            if (recorded & (1U << edit)) tx.setSelection({edit}, {edit + 10});
            tx.commit();
        }
        CHECK(doc.undoCount() == 1);
        REQUIRE(doc.undo());
        if (recorded) {
            const std::uint64_t first = recorded & 1U ? 0 : 1;
            CHECK(doc.restoredSelection() == std::optional<SelectionBlob>{{first}});
        } else CHECK_FALSE(doc.restoredSelection());
        REQUIRE(doc.redo());
        if (recorded) {
            const std::uint64_t last = recorded & 4U ? 12 : recorded & 2U ? 11 : 10;
            CHECK(doc.restoredSelection() == std::optional<SelectionBlob>{{last}});
        } else CHECK_FALSE(doc.restoredSelection());
    }
}

TEST_CASE("Document selection metadata counts toward history memory and eviction") {
    Document plain(parsed("entity { id 1 }"));
    Document selected(parsed("entity { id 1 }"));
    const auto edit = [](Document& doc, bool selection, const char* key) {
        auto tx = doc.begin("edit");
        tx.setKeyValue(*doc.handle(doc.roots()[0]), key, "value");
        if (selection) tx.setSelection(SelectionBlob(16, 1), SelectionBlob(32, 2));
        tx.commit();
    };
    edit(plain, false, "targetname");
    edit(selected, true, "targetname");
    const auto plainBytes = plain.historyBytes();
    const auto selectedBytes = selected.historyBytes();
    CHECK(selectedBytes == plainBytes + 48 * sizeof(std::uint64_t));
    edit(selected, true, "origin");
    selected.setUndoCap(selected.historyBytes() - selectedBytes);
    CHECK(selected.undoCount() == 1);
    REQUIRE(selected.undo());
    CHECK(selected.restoredSelection() == std::optional<SelectionBlob>{SelectionBlob(16, 1)});
    CHECK_FALSE(selected.undo());
    selected.setUndoCap(plainBytes);
    CHECK(selected.historyBytes() == 0);
    CHECK_FALSE(selected.redo());
}

TEST_CASE("Document coalesced selection memory is recalculated before eviction") {
    Document doc(parsed("entity { id 1 targetname original }"));
    const auto entity = *doc.handle(doc.roots()[0]);
    const auto edit = [&](const char* value, SelectionBlob before, SelectionBlob after) {
        auto tx = doc.begin("typing");
        tx.setKeyValue(entity, "targetname", value);
        tx.setSelection(std::move(before), std::move(after));
        tx.commit();
    };
    edit("a", {1}, {2});
    const auto initial = doc.historyBytes();
    edit("b", SelectionBlob(50, 3), SelectionBlob(10, 4));
    CHECK(doc.undoCount() == 1);
    CHECK(doc.historyBytes() == initial + 9 * sizeof(std::uint64_t));
    doc.setUndoCap(doc.historyBytes());
    edit("c", {5}, SelectionBlob(11, 6));
    CHECK(doc.undoCount() == 0);
    CHECK(doc.historyBytes() == 0);
}

TEST_CASE("Document selection-only commits stay silent and preserve history behavior") {
    const std::string bytes = "entity { id 1 targetname original }";
    Document doc(parsed(bytes));
    const auto entity = *doc.handle(doc.roots()[0]);
    std::size_t notifications = 0;
    doc.observe([&](const ChangeSet&) { ++notifications; });
    const auto selectionOnly = [&] {
        const auto history = doc.historyBytes();
        const auto undo = doc.undoCount(), redo = doc.redoCount();
        const auto observed = notifications;
        auto tx = doc.begin("selection only");
        tx.setSelection({1}, {2});
        CHECK(tx.commit().empty());
        CHECK(doc.historyBytes() == history);
        CHECK(doc.undoCount() == undo);
        CHECK(doc.redoCount() == redo);
        CHECK(notifications == observed);
        CHECK_FALSE(doc.restoredSelection());
    };
    selectionOnly();
    CHECK(saved(doc) == bytes);
    CHECK(doc.dirtyNodes().empty());
    {
        auto tx = doc.begin("edit");
        tx.setKeyValue(entity, "targetname", "a");
        tx.setSelection({3}, {4});
        tx.commit();
    }
    selectionOnly();
    {
        auto tx = doc.begin("coalesced edit");
        tx.setKeyValue(entity, "targetname", "b");
        tx.commit();
    }
    CHECK(doc.undoCount() == 1);
    REQUIRE(doc.undo());
    REQUIRE(doc.restoredSelection());
    selectionOnly();
    CHECK(saved(doc) == bytes);
    CHECK(doc.dirtyNodes().empty());
    REQUIRE(doc.redo());
    CHECK(doc.restoredSelection() == std::optional<SelectionBlob>{{4}});
    doc.begin("empty").commit();
    CHECK_FALSE(doc.restoredSelection());
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
        "entity { id 5 removable value targetname first group { id 8 } targetname second\n"
        "solid { id 9 side { id 20 } } editor { visgroupid 6 } }\n";
    Document doc(parsed(bytes));
    const auto entity = *doc.handle(doc.roots()[0]);
    auto tx = doc.begin("remove key before hierarchy slots");
    CHECK_THROWS_AS(tx.setKeyValue(entity, "TARGETNAME", {}), std::invalid_argument);
    CHECK_THROWS_AS(tx.setKeyValue(entity, "targetname", "first"), std::invalid_argument);
    CHECK_THROWS_AS(tx.setKeyValue(entity, "targetname", "changed"), std::invalid_argument);
    tx.setKeyValue(entity, "removable", {});
    tx.commit();
    auto output = doc.toVmf();
    REQUIRE(output);
    REQUIRE(output->entities.size() == 1);
    CHECK(output->entities[0].groups.size() == 1);
    CHECK(output->entities[0].solids.size() == 1);
    REQUIRE(output->entities[0].findAll("targetname").size() == 2);
    CHECK(*output->entities[0].findAll("targetname")[0]->text() == "first");
    CHECK(*output->entities[0].findAll("targetname")[1]->text() == "second");
    CHECK(output->entities[0].find("removable") == nullptr);
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

TEST_CASE("Document setEditor preserves membership order and restores exact bytes for every object") {
    for (const auto* type : {"solid", "entity", "group"}) {
        CAPTURE(type);
        const std::string bytes = std::string(type) +
            " { id 7 editor { color \"1 2 3\" groupid 4 VisgroupID 1 visgroupid 2 "
            "visgroupid 1 comments old custom untouched } }\n";
        Document doc(parsed(bytes));
        const auto object = *doc.handle(doc.roots()[0]);
        const auto editorBlock = [&]() -> const kv1::Node& {
            if (std::holds_alternative<Handle<Solid>>(object))
                return *doc.get(object)->block->find("editor");
            return *doc.get(*doc.handle(doc.get(object)->children[0]))->block;
        };
        const auto memberships = [&]() {
            std::vector<int> result;
            for (const auto* entry : editorBlock().findAll("visgroupid"))
                result.push_back(*entry->asInt());
            return result;
        };
        const auto edit = [&](EditorEdit operation, Dirty flags) {
            auto tx = doc.begin("editor edit");
            tx.setEditor(object, std::move(operation));
            const auto changes = tx.commit();
            REQUIRE(changes.modified.size() == 1);
            CHECK(changes.modified[0].handle == object);
            CHECK(changes.modified[0].flags == flags);
        };
        edit(EditorEdit::setGroup(9), Dirty::Hierarchy);
        CHECK(editorBlock().find("groupid")->asInt() == 9);
        edit(EditorEdit::setGroup({}), Dirty::Hierarchy);
        CHECK(editorBlock().find("groupid") == nullptr);
        edit(EditorEdit::addVisgroup(3), Dirty::Visibility);
        CHECK(memberships() == std::vector<int>{1, 2, 1, 3});
        edit(EditorEdit::addVisgroup(1), Dirty::Visibility);
        CHECK(memberships() == std::vector<int>{1, 2, 1, 3, 1});
        edit(EditorEdit::removeVisgroup(1), Dirty::Visibility);
        CHECK(memberships() == std::vector<int>{2, 1, 3, 1});
        edit(EditorEdit::setColor("20 30 40"), Dirty::KeyValues);
        edit(EditorEdit::setVisgroupShown(false), Dirty::Visibility);
        edit(EditorEdit::setVisgroupAutoShown(true), Dirty::Visibility);
        edit(EditorEdit::setComments("new comments"), Dirty::KeyValues);
        CHECK(editorBlock().find("color")->decodedValue() == "20 30 40");
        CHECK(editorBlock().find("visgroupshown")->asInt() == 0);
        CHECK(editorBlock().find("visgroupautoshown")->asInt() == 1);
        CHECK(editorBlock().find("comments")->decodedValue() == "new comments");
        CHECK(editorBlock().find("custom")->decodedValue() == "untouched");
        {
            auto tx = doc.begin("missing membership");
            tx.setEditor(object, EditorEdit::removeVisgroup(999));
            tx.setEditor(object, EditorEdit::setGroup({}));
            CHECK(tx.commit().empty());
        }
        const auto edited = saved(doc);
        while (doc.undo()) {}
        CHECK(saved(doc) == bytes);
        CHECK(doc.dirtyNodes().empty());
        while (doc.redo()) {}
        CHECK(saved(doc) == edited);
    }
}

TEST_CASE("Document setEditor creates missing editor blocks and rolls back silently") {
    for (const auto* type : {"solid", "entity", "group"}) {
        CAPTURE(type);
        const std::string bytes = std::string(type) + " { id 7 }\n";
        Document doc(parsed(bytes));
        const auto object = *doc.handle(doc.roots()[0]);
        {
            auto tx = doc.begin("cancel editor creation");
            tx.setEditor(object, EditorEdit::addVisgroup(8));
            tx.setEditor(object, EditorEdit::setComments("cancel"));
        }
        CHECK(saved(doc) == bytes);
        {
            auto tx = doc.begin("create editor");
            tx.setEditor(object, EditorEdit::addVisgroup(8));
            tx.setEditor(object, EditorEdit::setGroup(2));
            const auto changes = tx.commit();
            REQUIRE(changes.modified.size() == 1);
            CHECK(changes.modified[0].handle == object);
            CHECK(changes.modified[0].flags == (Dirty::Visibility | Dirty::Hierarchy));
        }
        const auto edited = saved(doc);
        CHECK(edited.find("visgroupid") != std::string::npos);
        REQUIRE(doc.undo());
        CHECK(saved(doc) == bytes);
        REQUIRE(doc.redo());
        CHECK(saved(doc) == edited);
    }
}

TEST_CASE("Document duplicate side index follows subtree ordering and deletion") {
    const std::string bytes = "world { id 1 group { id 2 solid { id 3 side { id 10 } } } "
                              "solid { id 4 side { id 10 } side { id 10 } } }";
    Document doc(parsed(bytes));
    const auto world = *doc.handle(doc.roots()[0]);
    const auto group = *doc.handle(doc.get(world)->children[0]);
    const auto first = doc.side(10)->solid;
    const auto second = std::get<Handle<Solid>>(*doc.handle(doc.get(world)->children[1]));
    {
        auto tx = doc.begin("move duplicate subtree");
        tx.reparent(group, {}, 1);
        CHECK(doc.side(10)->solid == second);
        CHECK(doc.side(10)->faceIndex == 0);
        tx.commit();
    }
    {
        auto tx = doc.begin("destroy duplicate winner");
        tx.destroy(second);
        CHECK(doc.side(10)->solid == first);
        CHECK(tx.commit().vanishedSides.empty());
    }
    REQUIRE(doc.undo());
    CHECK(doc.side(10)->solid != first);
    REQUIRE(doc.undo());
    CHECK(doc.side(10)->solid == first);
    CHECK(saved(doc) == bytes);
}

TEST_CASE("Document editor removals retain nested unknown blocks and object undo flags") {
    const std::string bytes = "entity { id 1 editor { groupid 4 custom { odd raw } "
                              "visgroupid 2 tail { key value } visgroupid 3 } }\n";
    Document doc(parsed(bytes));
    const auto object = *doc.handle(doc.roots()[0]);
    ChangeSet observed;
    doc.observe([&](const ChangeSet& changes) { observed = changes; });
    {
        auto tx = doc.begin("remove editor keys");
        tx.setEditor(object, EditorEdit::setGroup({}));
        tx.setEditor(object, EditorEdit::removeVisgroup(2));
        tx.commit();
    }
    const auto edited = saved(doc);
    auto output = doc.toVmf();
    REQUIRE(output);
    REQUIRE(output->entities[0].editor);
    const auto& editor = *output->entities[0].editor;
    CHECK(editor.find("groupid") == nullptr);
    CHECK(editor.findAll("visgroupid").size() == 1);
    CHECK(editor.node->find("custom")->find("odd")->decodedValue() == "raw");
    CHECK(editor.node->find("tail")->find("key")->decodedValue() == "value");
    REQUIRE(doc.undo());
    REQUIRE(observed.modified.size() == 1);
    CHECK(observed.modified[0].handle == object);
    CHECK(observed.modified[0].flags == (Dirty::Hierarchy | Dirty::Visibility));
    CHECK(saved(doc) == bytes);
    REQUIRE(doc.redo());
    REQUIRE(observed.modified.size() == 1);
    CHECK(observed.modified[0].handle == object);
    CHECK(observed.modified[0].flags == (Dirty::Hierarchy | Dirty::Visibility));
    CHECK(saved(doc) == edited);
}

TEST_CASE("Document setKeyValue rejects duplicate keys before no-op or removal") {
    const std::string bytes = "entity { id 1 editor { VisgroupID 2 visgroupid 3 } }\n";
    Document doc(parsed(bytes));
    const auto object = *doc.handle(doc.roots()[0]);
    const auto editor = *doc.handle(doc.get(object)->children[0]);
    auto tx = doc.begin("ambiguous key edits");
    CHECK_THROWS_AS(tx.setKeyValue(editor, "visgroupid", "4"), std::invalid_argument);
    CHECK_THROWS_AS(tx.setKeyValue(editor, "VISGROUPID", "2"), std::invalid_argument);
    CHECK_THROWS_AS(tx.setKeyValue(editor, "visgroupid", {}), std::invalid_argument);
    CHECK(tx.commit().empty());
    CHECK(doc.undoCount() == 0);
    CHECK(saved(doc) == bytes);
}

TEST_CASE("Document history labels and jumps match repeated undo and redo") {
    const auto file = parsed("entity { id 1 classname info_target }\n");
    Document jumped(file), stepped(file);
    const auto populate = [](Document& doc) {
        const auto entity = *doc.handle(doc.roots()[0]);
        for (int i = 1; i <= 4; ++i) {
            auto tx = doc.begin("Edit " + std::to_string(i));
            tx.setKeyValue(entity, "targetname", std::to_string(i));
            tx.setSelection({static_cast<std::uint64_t>(i - 1)}, {static_cast<std::uint64_t>(i)});
            tx.commit(false);
        }
    };
    CHECK(jumped.undoLabels().empty());
    CHECK(jumped.redoLabels().empty());
    CHECK(jumped.jumpTo(0));
    populate(jumped);
    populate(stepped);
    CHECK(jumped.undoLabels() == std::vector<std::string>{"Edit 1", "Edit 2", "Edit 3", "Edit 4"});
    const auto memory = jumped.historyBytes();
    std::size_t notifications = 0;
    jumped.observe([&](const ChangeSet&) { ++notifications; });
    std::size_t expectedNotifications = 0;
    for (const std::size_t target : {2, 0, 3, 1, 4}) {
        expectedNotifications += target > stepped.undoCount() ? target - stepped.undoCount() : stepped.undoCount() - target;
        while (stepped.undoCount() > target) REQUIRE(stepped.undo());
        while (stepped.undoCount() < target) REQUIRE(stepped.redo());
        REQUIRE(jumped.jumpTo(target));
        CHECK(jumped.undoCount() == target);
        CHECK(saved(jumped) == saved(stepped));
        CHECK(jumped.restoredSelection() == stepped.restoredSelection());
        CHECK(jumped.historyBytes() == memory);
        CHECK(notifications == expectedNotifications);
    }
    REQUIRE(jumped.jumpTo(1));
    CHECK(jumped.undoLabels() == std::vector<std::string>{"Edit 1"});
    CHECK(jumped.redoLabels() == std::vector<std::string>{"Edit 2", "Edit 3", "Edit 4"});
    const auto before = saved(jumped);
    const auto selection = jumped.restoredSelection();
    const auto beforeNotifications = notifications;
    CHECK_FALSE(jumped.jumpTo(5));
    CHECK_FALSE(jumped.jumpTo(static_cast<std::size_t>(-1)));
    CHECK(saved(jumped) == before);
    CHECK(jumped.undoCount() == 1);
    CHECK(jumped.redoCount() == 3);
    CHECK(jumped.restoredSelection() == selection);
    CHECK(notifications == beforeNotifications);
}
