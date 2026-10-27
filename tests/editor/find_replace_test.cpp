#include "fixtures.hpp"
#include "editor/find_replace.hpp"

using namespace editor_test;

TEST_CASE("Keyvalue replace connections preserves ESC comma fields preview and byte identical undo") {
    const std::string source = "// fixture\nentity { id 1 classname logic_relay targetname old custom old old_key old "
        "connections { OnTrigger \"old,Open,hold,0.50,-1\" "
        "OnTrigger \"old\x1b" "Open\x1b" "hold,old\x1b" "0.50\x1b-1\" "
        "Broken \"old,broken\" } }\nentity { id 2 targetname old }";
    Document doc(parsed(source));
    EditorSession session;
    session.attach(doc);
    const std::set<NodeId> scope{doc.roots()[0]};
    KeyvalueReplaceOptions options;
    options.find = "old"; options.replacement = "new"; options.wholeWord = true; options.connections = true;
    auto preview = previewKeyvalueReplace(doc, scope, options);
    REQUIRE(preview);
    CHECK(preview->size() == 5); // targetname, custom, old_key value, two connections
    const auto history = doc.undoCount();
    REQUIRE(applyKeyvalueReplace(session, *preview));
    CHECK(doc.undoCount() == history + 1);
    for (const auto& change : *preview) {
        const auto* node = doc.get(*doc.handle(change.connectionBlock.value_or(change.entity)));
        const auto& entry = node->block->children()[change.index];
        CHECK(entry.key() == change.newKey);
        CHECK(entry.decodedValue() == change.newValue);
    }
    auto written = doc.write().value();
    CHECK(written.contains("new,Open,hold,0.50,-1"));
    CHECK(written.contains("new\x1b" "Open\x1b" "hold,new\x1b" "0.50\x1b-1"));
    CHECK(written.contains("old,broken"));
    CHECK(doc.get(*doc.handle(doc.roots()[1]))->block->find("targetname")->decodedValue() == "old");
    REQUIRE(doc.undo());
    CHECK(doc.write().value() == source);
}

TEST_CASE("Keyvalue replace supports keys duplicates case wildcard whole words and stale rollback") {
    Document doc(parsed("entity { id 1 targetname OLD old_key old custom old custom old suffix older wild \"door_one door_two\" }"));
    EditorSession session; session.attach(doc);
    const std::set<NodeId> ids{doc.roots()[0]};
    const auto source = doc.write().value();
    KeyvalueReplaceOptions options;
    options.find = "old"; options.replacement = "new"; options.wholeWord = true;
    options.caseSensitive = true;
    auto preview = previewKeyvalueReplace(doc, ids, options);
    REQUIRE(preview); CHECK(preview->size() == 3);
    options.caseSensitive = false;
    preview = previewKeyvalueReplace(doc, ids, options);
    REQUIRE(preview); CHECK(preview->size() == 4);
    REQUIRE(applyKeyvalueReplace(session, *preview));
    CHECK(doc.get(*doc.handle(doc.roots()[0]))->block->find("old_key")->decodedValue() == "new");
    REQUIRE(doc.undo()); CHECK(doc.write().value() == source);
    options.wholeWord = false; options.values = false;
    preview = previewKeyvalueReplace(doc, ids, options);
    REQUIRE(preview); REQUIRE(preview->size() == 1);
    CHECK((*preview)[0].newKey == "new_key");
    REQUIRE(applyKeyvalueReplace(session, *preview));
    CHECK(doc.get(*doc.handle(doc.roots()[0]))->block->find("new_key") != nullptr);
    REQUIRE(doc.undo()); CHECK(doc.write().value() == source);
    options.values = true; options.keys = false; options.wildcard = true; options.find = "door_*";
    preview = previewKeyvalueReplace(doc, ids, options);
    REQUIRE(preview); REQUIRE(preview->size() == 1);
    CHECK((*preview)[0].newValue == "new"); // greedy '*' across the field
    options.wildcard = false; options.find = "old";
    preview = previewKeyvalueReplace(doc, ids, options); REQUIRE(preview);
    auto stale = *preview; stale.back().oldValue = "stale";
    CHECK_FALSE(applyKeyvalueReplace(session, stale));
    CHECK(doc.write().value() == source);
    CHECK(doc.undoCount() == 0);
    CHECK(previewKeyvalueReplace(doc, {}, options)->empty());
}

TEST_CASE("Keyvalue replace targetname is plain text and separator insertion is refused") {
    Document doc(parsed("entity { targetname old connections { OnTrigger \"old,Open,,0,-1\" } }"));
    EditorSession session; session.attach(doc);
    KeyvalueReplaceOptions options;
    options.find = "old"; options.replacement = "new";
    auto changes = previewKeyvalueReplace(doc, {doc.roots()[0]}, options);
    REQUIRE(changes); REQUIRE(applyKeyvalueReplace(session, *changes));
    CHECK(doc.write().value().contains("old,Open,,0,-1"));
    REQUIRE(doc.undo());
    options.connections = true; options.replacement = "bad,name";
    CHECK_FALSE(previewKeyvalueReplace(doc, {doc.roots()[0]}, options));
    options.find = "id"; options.replacement = "changed";
    CHECK(previewKeyvalueReplace(doc, {doc.roots()[0]}, options)->empty());
}

TEST_CASE("Material replace selected faces only whole map substring entity keys and one undo step") {
    Document doc(parsed("entity { classname fixture material \"addon/wall\" texture \"addon/wall_trim\" }"));
    const auto a = addBox(doc, {0,0,0}, {32,32,32});
    const auto b = addBox(doc, {64,0,0}, {96,32,32});
    std::vector<SideId> faces;
    for (const auto h : {a,b}) for (const auto& side : doc.get(h)->block->children())
        if (side.key() == "side") faces.push_back(std::stoi(side.find("id")->decodedValue().value()));
    { auto tx = doc.begin("fixture materials"); for (auto face : faces) tx.setSideKeyValue(face, "material", "Addon/Wall"); tx.commit(false); }
    EditorSession session; session.attach(doc);
    session.selection().setMode(SelectionMode::Faces);
    session.selection().add(faces[0]); session.selection().add(faces[1]);
    const auto before = doc.write().value();
    const auto history = doc.undoCount();
    MaterialReplaceOptions options; options.find = "addon\\wall"; options.replacement = "fixture/replaced"; options.entityKeys = true;
    auto changes = previewMaterialReplace(session, options);
    REQUIRE(changes); REQUIRE(changes->size() == 2);
    REQUIRE(applyMaterialReplace(session, *changes));
    CHECK(doc.undoCount() == history + 1);
    auto remaining = previewMaterialReplace(session, options); REQUIRE(remaining); CHECK(remaining->empty());
    options.scope = MaterialReplaceScope::WholeMap;
    remaining = previewMaterialReplace(session, options); REQUIRE(remaining); CHECK(remaining->size() == 11); // 10 faces + material key
    REQUIRE(doc.undo()); CHECK(doc.write().value() == before);
    changes = previewMaterialReplace(session, options); REQUIRE(changes); CHECK(changes->size() == 13);
    REQUIRE(applyMaterialReplace(session, *changes));
    CHECK(doc.undoCount() == history + 1);
    REQUIRE(doc.undo()); CHECK(doc.write().value() == before);
    options.wholeName = false; options.find = "wall"; options.replacement = "floor";
    changes = previewMaterialReplace(session, options); REQUIRE(changes); CHECK(changes->size() == 14);
    CHECK(changes->back().newValue == "Addon/floor");
    REQUIRE(applyMaterialReplace(session, *changes));
    CHECK(doc.get(*doc.handle(doc.roots()[0]))->block->find("texture")->decodedValue() == "addon/floor_trim");
    REQUIRE(doc.undo()); CHECK(doc.write().value() == before);
    session.selection().setMode(SelectionMode::Solids); session.selection().add(id(doc, a));
    options.scope = MaterialReplaceScope::Selection;
    changes = previewMaterialReplace(session, options); REQUIRE(changes); CHECK(changes->size() == 6);
    auto stale = *changes; stale.back().oldValue = "stale";
    CHECK_FALSE(applyMaterialReplace(session, stale)); CHECK(doc.write().value() == before);
}

TEST_CASE("Material replace recognizes FGD typed entity keys and empty previews add no undo") {
    Document doc(parsed("entity { classname fixture custommat \"fixture/wall\" material \"fixture/wall\" message \"fixture/wall\" }"));
    EditorSession session; session.attach(doc);
    assets::EntityClass cls;
    cls.name = "fixture";
    assets::EntityProperty custom, prose;
    custom.name = "custommat"; custom.type = "material";
    prose.name = "material"; prose.type = "string";
    cls.properties = {custom, prose};
    assets::EntityClassTable classes; classes.classes.emplace("fixture", cls);
    session.setEntityClasses(std::move(classes));
    session.selection().setMode(SelectionMode::Objects);
    session.selection().add(doc.roots()[0]);
    MaterialReplaceOptions options;
    options.find = "fixture/wall"; options.replacement = "fixture/floor"; options.entityKeys = true;
    auto changes = previewMaterialReplace(session, options);
    REQUIRE(changes); REQUIRE(changes->size() == 1);
    CHECK(changes->front().key == "custommat");
    const auto before = doc.write().value();
    REQUIRE(applyMaterialReplace(session, *changes));
    CHECK(doc.undoCount() == 1);
    REQUIRE(doc.undo()); CHECK(doc.write().value() == before);
    options.find = "absent";
    changes = previewMaterialReplace(session, options);
    REQUIRE(changes); CHECK(changes->empty());
    REQUIRE(applyMaterialReplace(session, *changes));
    CHECK(doc.undoCount() == 0);
    KeyvalueReplaceOptions keys; keys.find = "absent";
    const auto keyChanges = previewKeyvalueReplace(doc, {doc.roots()[0]}, keys);
    REQUIRE(keyChanges); REQUIRE(applyKeyvalueReplace(session, *keyChanges));
    CHECK(doc.undoCount() == 0);
    CHECK(doc.write().value() == before);
}
