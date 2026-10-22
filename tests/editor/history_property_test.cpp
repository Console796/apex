#include "fixtures.hpp"
#include "editor/editor_session.hpp"

#include <array>
#include <numeric>
#include <random>

using namespace editor_test;

namespace {
std::string saved(const Document& doc) {
    const auto result = doc.write();
    REQUIRE(result);
    return *result;
}
void validSelection(const Document& doc, const EditorSession& session) {
    for (const auto& target : session.selection().ordered()) {
        if (const auto* node = std::get_if<NodeId>(&target)) CHECK(doc.handle(*node).has_value());
        else CHECK(doc.side(std::get<SideId>(target)).has_value());
    }
}
void editName(EditorSession& session, std::string value, std::string focus = {}) {
    const auto model = session.inspectorModel(true);
    const auto row = std::ranges::find(model.rows(), "targetname", &InspectorModel::Row::key);
    REQUIRE(row != model.rows().end());
    REQUIRE(session.editProperty(static_cast<std::size_t>(row - model.rows().begin()), std::move(value), std::move(focus), true));
}
struct Replay {
    Document doc{parsed(R"(world { id 1 classname worldspawn }
        entity { id 2 classname info_target origin "0 0 0" targetname first }
        entity { id 3 classname info_target origin "64 0 0" targetname second })")};
    EditorSession session;
    int visgroup = 0;
    Replay() {
        for (int i = 0; i < 3; ++i) addBox(doc, {i * 32, 0, 0}, {i * 32 + 16, 16, 16}, worldHandle(doc));
        session.attach(doc);
        assets::EntityClassTable table;
        assets::EntityClass cls;
        cls.name = "info_target";
        cls.type = "PointClass";
        table.classes[cls.name] = cls;
        session.setEntityClasses(std::move(table));
        session.selection().setMode(SelectionMode::Objects);
        session.selection().setIgnoreGroups(true);
        visgroup = session.createVisgroup("property").value();
        doc.setUndoCap(0);
        doc.setUndoCap(64 * 1024 * 1024);
    }
    void step(unsigned operation, std::mt19937& rng, int serial) {
        std::vector<NodeId> objects, entities, groups;
        for (const auto h : nodes(doc)) {
            const auto* n = doc.get(h);
            if (std::holds_alternative<Handle<Group>>(h)) groups.push_back(n->id);
            if (n->block->key() == "entity") entities.push_back(n->id);
            if (n->block->key() == "entity" || std::holds_alternative<Handle<Solid>>(h)) objects.push_back(n->id);
        }
        REQUIRE(objects.size() >= 2);
        REQUIRE_FALSE(entities.empty());
        session.selection().select(objects[rng() % objects.size()]);
        if (operation <= 1) {
            // Transform tools intentionally reject hidden targets. Reveal the chosen
            // object through the public API before exercising a valid transform.
            const auto chosen = std::get<NodeId>(session.selection().ordered()[0]);
            if (session.nodeHidden(chosen)) REQUIRE(session.setNodeHidden({chosen}, false));
            session.selection().select(chosen);
        }
        switch (operation) {
        case 0: REQUIRE(session.moveSelection({8, -4, 2}, true)); break;
        case 1: REQUIRE(session.rotateSelection({0, 0, 90})); break;
        case 2: REQUIRE(session.duplicateSelection()); break;
        case 3:
            if (objects.size() > 5 && entities.size() > 2) REQUIRE(session.deleteSelection());
            else REQUIRE(session.duplicateSelection());
            break;
        case 4:
            session.selection().add(objects[(rng() % (objects.size() - 1) + 1) % objects.size()]);
            REQUIRE(session.groupSelection());
            break;
        case 5:
            if (!groups.empty()) {
                session.selection().select(groups[rng() % groups.size()]);
                REQUIRE(session.ungroupSelection());
            } else REQUIRE(session.groupSelection());
            break;
        case 6: REQUIRE(session.hideSelection()); break;
        case 7: REQUIRE(session.unhideAll()); break;
        case 8:
            session.selection().select(entities[rng() % entities.size()]);
            REQUIRE(session.renameEntity(std::get<NodeId>(session.selection().ordered()[0]), "rename_" + std::to_string(serial)));
            break;
        case 9: {
            session.selection().select(entities[rng() % entities.size()]);
            const auto entity = std::get<NodeId>(session.selection().ordered()[0]);
            if (!doc.get(*doc.handle(entity))->block->find("targetname"))
                REQUIRE(session.renameEntity(entity, "typing-start"));
            const auto before = doc.undoCount();
            const auto focus = "focus_" + std::to_string(serial);
            for (int i = 0; i < 3; ++i) editName(session, "typed_" + std::to_string(serial) + '_' + std::to_string(i), focus);
            CHECK(doc.undoCount() == before + 1);
            break;
        }
        case 10: {
            const auto text = session.copyText();
            REQUIRE(text);
            REQUIRE(session.pasteText(*text));
            break;
        }
        case 11: {
            const auto text = session.cutText();
            REQUIRE(text);
            REQUIRE(session.pasteText(*text));
            break;
        }
        case 12: REQUIRE(session.addSelectionToVisgroup(visgroup)); break;
        case 13: REQUIRE(session.removeSelectionFromVisgroup(visgroup)); break;
        case 14: REQUIRE(session.setVisgroupShown(visgroup, serial % 2 == 0)); break;
        case 15: REQUIRE(session.placeEntity("info_target", DVec3{serial * 8, 32, 0}, std::nullopt)); break;
        }
        validSelection(doc, session);
    }
};
}

TEST_CASE("EditorSession seeded mixed edits replay byte exactly through undo redo and branching") {
    for (const unsigned seed : {0xA9E2U, 0x210U, 0xC0FFEEU}) {
        CAPTURE(seed);
        Replay replay;
        auto& doc = replay.doc;
        auto& session = replay.session;
        std::mt19937 rng(seed);
        std::vector<std::string> states{saved(doc)};
        // An observer records every transaction, including both halves of cut/paste.
        const auto observer = doc.observe([&](const ChangeSet&) {
            const auto count = doc.undoCount();
            REQUIRE(count <= states.size());
            states.resize(count + 1);
            states[count] = saved(doc);
        });
        std::array<unsigned, 16> operations;
        std::iota(operations.begin(), operations.end(), 0U);
        for (int i = 0; i < 208; ++i) {
            CAPTURE(i);
            if (i % 16 == 0) std::shuffle(operations.begin(), operations.end(), rng);
            CAPTURE(operations[i % 16]);
            replay.step(operations[i % 16], rng, i);
        }
        doc.unobserve(observer);
        REQUIRE(states.size() == doc.undoCount() + 1);
        const auto memory = doc.historyBytes();
        REQUIRE(memory > 0);
        for (std::size_t i = states.size() - 1; i > 0; --i) {
            CAPTURE(i);
            REQUIRE(session.perform("edit.undo"));
            REQUIRE(saved(doc) == states[i - 1]);
            validSelection(doc, session);
            // Old entries are compacted and the window thawed lazily, so usage moves a little.
            CHECK(doc.historyBytes() > 0);
            CHECK(doc.historyBytes() <= memory * 2);
        }
        CHECK_FALSE(session.perform("edit.undo"));
        for (std::size_t i = 1; i < states.size(); ++i) {
            REQUIRE(session.perform("edit.redo"));
            REQUIRE(saved(doc) == states[i]);
            validSelection(doc, session);
            // Old entries are compacted and the window thawed lazily, so usage moves a little.
            CHECK(doc.historyBytes() > 0);
            CHECK(doc.historyBytes() <= memory * 2);
        }
        CHECK_FALSE(session.perform("edit.redo"));
        // Several branch points, each followed by jumps across the surviving history.
        for (int branch = 0; branch < 5; ++branch) {
            const auto pivot = 1 + rng() % (states.size() - 2);
            REQUIRE(session.jumpHistory(pivot));
            REQUIRE(saved(doc) == states[pivot]);
            validSelection(doc, session);
            session.selection().clear();
            REQUIRE(session.placeEntity("info_target", DVec3{branch * 16, 128, 0}, std::nullopt));
            CHECK(doc.redoCount() == 0);
            CHECK_FALSE(session.perform("edit.redo"));
            states.resize(pivot + 1);
            states.push_back(saved(doc));
            const auto branchedMemory = doc.historyBytes();
            REQUIRE(session.jumpHistory(0));
            CHECK(saved(doc) == states.front());
            REQUIRE(session.jumpHistory(states.size() - 1));
            CHECK(saved(doc) == states.back());
            CHECK(doc.historyBytes() > 0);
            CHECK(doc.historyBytes() <= branchedMemory * 2);
            validSelection(doc, session);
        }
    }
}

TEST_CASE("Undo cap preserves exact newest states and redo eviction horizon") {
    Document doc(parsed("entity { id 1 classname info_target targetname initial }"));
    EditorSession session;
    session.attach(doc);
    session.selection().select(doc.roots().front());
    std::vector<std::string> states{saved(doc)};
    std::vector<std::size_t> costs;
    for (int i = 0; i < 8; ++i) {
        const auto before = doc.historyBytes();
        editName(session, "state_" + std::to_string(i));
        costs.push_back(doc.historyBytes() - before);
        states.push_back(saved(doc));
    }
    // Shared blocks make per-entry cost non-additive; this cap keeps exactly three entries.
    const auto cap = costs[4] + costs[5] + costs[6] + costs[7] - costs[7] / 4;
    doc.setUndoCap(cap);
    REQUIRE(doc.undoCount() == 3);
    CHECK(doc.historyBytes() <= cap);
    for (int i = 7; i >= 5; --i) {
        REQUIRE(session.perform("edit.undo"));
        CHECK(saved(doc) == states[static_cast<std::size_t>(i)]);
    }
    CHECK_FALSE(doc.undo());
    CHECK(saved(doc) == states[5]);
    for (std::size_t i = 6; i <= 8; ++i) {
        REQUIRE(session.perform("edit.redo"));
        CHECK(saved(doc) == states[i]);
    }
    CHECK_FALSE(doc.redo());
    REQUIRE(session.perform("edit.undo"));
    REQUIRE(session.perform("edit.undo"));
    CHECK(saved(doc) == states[6]);
    editName(session, "branch");
    const auto branch = saved(doc);
    CHECK(doc.redoCount() == 0);
    CHECK_FALSE(doc.redo());
    REQUIRE(session.perform("edit.undo"));
    CHECK(saved(doc) == states[6]);
    REQUIRE(session.perform("edit.redo"));
    CHECK(saved(doc) == branch);
    REQUIRE(session.jumpHistory(0));
    // Shrinking while all entries are on redo must evict safely too.
    doc.setUndoCap(1);
    CHECK(doc.historyBytes() == 0);
    CHECK_FALSE(doc.undo());
    CHECK_FALSE(doc.redo());
    const auto horizon = saved(doc);
    CHECK_FALSE(session.jumpHistory(1));
    CHECK(saved(doc) == horizon);
}

TEST_CASE("Inspector multiselect typing coalesces only within the same focus and selection") {
    Document doc(parsed(R"(entity { id 1 classname info_target targetname alpha custom "keep A" }
        entity { id 2 classname info_target targetname beta custom "keep B" })"));
    EditorSession session;
    session.attach(doc);
    const auto a = doc.roots()[0], b = doc.roots()[1];
    session.selection().add(a);
    session.selection().add(b);
    const auto before = saved(doc);
    const auto selection = session.selection().toBlob();
    editName(session, "c", "shared-focus");
    editName(session, "co", "shared-focus");
    editName(session, "common", "shared-focus");
    CHECK(doc.undoCount() == 1);
    for (const auto node : {a, b}) CHECK(doc.get(*doc.handle(node))->block->find("targetname")->decodedValue() == "common");
    const auto common = saved(doc);
    session.selection().select(a);
    editName(session, "only-a", "shared-focus");
    CHECK(doc.undoCount() == 2);
    REQUIRE(session.perform("edit.undo"));
    CHECK(saved(doc) == common);
    REQUIRE(session.perform("edit.undo"));
    CHECK(saved(doc) == before);
    CHECK(session.selection().toBlob() == selection);
    REQUIRE(session.perform("edit.redo"));
    CHECK(saved(doc) == common);
    editName(session, "new-focus", "new-focus");
    CHECK(doc.undoCount() == 2);
    CHECK(doc.redoCount() == 0);
}
