#include "fixtures.hpp"

using namespace editor_test;

TEST_CASE("Visibility hidden wrapper helper agrees with render mirror ancestry rule") {
    Document doc(parsed());
    const auto world = addBlock(doc, "world { classname worldspawn }");
    const auto visible = addBox(doc, DVec3{-8}, DVec3{8}, world);
    const auto wrapper = addBlock(doc, "hidden {}", world);
    const auto group = addBlock(doc, "group {}", wrapper);
    const auto hidden = addBox(doc, DVec3{-8}, DVec3{8}, group);
    const auto entity = addBlock(doc, "entity { classname func_detail }", wrapper);
    addBox(doc, DVec3{-4}, DVec3{4}, entity);
    for (const auto h : nodes(doc)) {
        const auto* node = doc.get(h);
        if (h == wrapper) continue; // The exported helper also hides the wrapper itself.
        bool mirrorHidden = false;
        for (auto parent = node->parent; parent;) {
            const auto* ancestor = doc.get(*doc.handle(parent));
            if (ancestor->block && ancestor->block->key() == "hidden") mirrorHidden = true;
            parent = ancestor->parent;
        }
        CHECK(isHiddenByWrapper(doc, node->id) == mirrorHidden);
        CHECK(isVisible(doc, node->id) == !mirrorHidden);
    }
    CHECK(isVisible(doc, id(doc, visible)));
    CHECK_FALSE(isVisible(doc, id(doc, hidden)));
    CHECK(isHiddenByWrapper(doc, id(doc, wrapper)));
    CHECK_FALSE(isVisible(doc, 0));
    CHECK_FALSE(isVisible(doc, 999999));
    PickIndex index(doc);
    CHECK(index.size() == 1);
}

TEST_CASE("Visibility entity child and solid inline editor states hide descendants and picks") {
    for (const bool automatic : {false, true}) {
        Document doc(parsed());
        const auto entity = addBlock(doc, "entity { classname func_detail editor { visgroupshown 1 } }");
        const auto solid = addBox(doc, DVec3{-8}, DVec3{8}, entity);
        PickIndex index(doc);
        index.attach(doc);
        const Ray ray{{-20, 0, 0}, {1, 0, 0}};
        REQUIRE(index.first(ray));
        for (const auto target : {solid, entity}) {
            auto tx = doc.begin("hide");
            tx.setEditor(target, automatic ? EditorEdit::setVisgroupAutoShown(false) :
                                            EditorEdit::setVisgroupShown(false));
            tx.commit();
            CHECK_FALSE(isVisible(doc, id(doc, solid)));
            CHECK_FALSE(index.first(ray));
            REQUIRE(doc.undo());
            CHECK(isVisible(doc, id(doc, solid)));
            CHECK(index.first(ray));
            REQUIRE(doc.redo());
            CHECK_FALSE(index.first(ray));
            REQUIRE(doc.undo());
        }
    }
}

TEST_CASE("Visibility isolate shows selected subtrees and preserves file hiding") {
    Document doc(parsed());
    const auto group = addBlock(doc, "group {}");
    const auto a = addBox(doc, DVec3{-8}, DVec3{8}, group);
    const auto b = addBox(doc, DVec3{20}, DVec3{24});
    const auto hidden = addBlock(doc, "hidden {}", group);
    const auto c = addBox(doc, DVec3{-4}, DVec3{4}, hidden);
    PickIndex index(doc);
    ViewState view;
    view.isolate.emplace(std::initializer_list<NodeId>{id(doc, group)});
    index.setViewState(view);
    CHECK(isVisible(doc, id(doc, a), view));
    CHECK_FALSE(isVisible(doc, id(doc, b), view));
    CHECK_FALSE(isVisible(doc, id(doc, c), view));
    CHECK_FALSE(isVisible(doc, id(doc, group), ViewState{{{id(doc, a)}}}));
    CHECK(index.size() == 1);
    REQUIRE(index.first({{-20, 0, 0}, {1, 0, 0}}));
    view.isolate->clear();
    index.setViewState(view);
    CHECK(index.pick({{-20, 0, 0}, {1, 0, 0}}).empty());
    index.setViewState({});
    CHECK(index.size() == 2);
}

TEST_CASE("Visibility point entity and direct editor child edits refresh picking") {
    Document doc(parsed());
    const auto point = addBlock(doc, "entity { classname info_target editor { visgroupshown 1 } }");
    const auto pointId = id(doc, point);
    const auto editorId = doc.get(point)->children[0];
    PickIndex index(doc);
    index.attach(doc);
    const Ray ray{{-20, 0, 0}, {1, 0, 0}};
    REQUIRE(index.first(ray));
    auto tx = doc.begin("direct visibility edit");
    tx.setKeyValue(*doc.handle(editorId), "visgroupshown", "0");
    tx.commit();
    CHECK_FALSE(isVisible(doc, pointId));
    CHECK_FALSE(index.first(ray));
    REQUIRE(doc.undo());
    CHECK(isVisible(doc, pointId));
    CHECK(index.first(ray));
    const auto wrapper = addBlock(doc, "hidden {}");
    auto hide = doc.begin("hide point");
    hide.reparent(point, wrapper);
    hide.commit();
    CHECK_FALSE(index.first(ray));
    REQUIRE(doc.undo());
    CHECK(index.first(ray));
}
