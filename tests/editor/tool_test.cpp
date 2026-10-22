#include "fixtures.hpp"
#include "editor/editor_session.hpp"

using namespace editor_test;

TEST_CASE("tool actions route clicks to selection or one undoable placement") {
    Document doc(parsed());
    const auto box = id(doc, addBox(doc, {-16, -16, -16}, {16, 16, 0}));
    EditorSession session;
    session.attach(doc);
    assets::EntityClassTable table;
    assets::EntityClass cls;
    cls.name = "test";
    cls.type = "PointClass";
    table.classes[cls.name] = cls;
    session.setEntityClasses(std::move(table));
    const Ray ray{{0, 0, 100}, {0, 0, -1}};
    CHECK(session.activeTool() == EditorSession::Tool::Select);
    session.pointerClick(ray);
    CHECK(session.selection().contains(box));
    const auto history = doc.undoCount();
    REQUIRE(session.perform("tool.entity"));
    CHECK_FALSE(session.gizmoContext());
    for (const auto* invalid : {"", "unknown"}) {
        session.setCurrentEntityClass(invalid);
        session.pointerClick(ray);
        CHECK(doc.undoCount() == history);
        CHECK(session.selection().contains(box));
    }
    session.pointerHover(ray);
    CHECK_FALSE(session.hoverTarget());
    session.marquee(planes({100,100,100}, {200,200,200}));
    CHECK(session.selection().contains(box));
    session.setCurrentEntityClass("test");
    session.setAlignToSurface(true);
    session.pointerClick(ray);
    CHECK(doc.undoCount() == history + 1);
    REQUIRE(session.selection().ordered().size() == 1);
    const auto entity = std::get<NodeId>(*session.selection().active());
    CHECK(entity != box);
    CHECK(doc.get(*doc.handle(entity))->block->find("angles"));
    REQUIRE(session.perform("edit.undo"));
    CHECK_FALSE(doc.handle(entity));
    CHECK(session.selection().contains(box));
    REQUIRE(session.perform("tool.select"));
    CHECK(session.activeTool() == EditorSession::Tool::Select);
    session.pointerClick(Ray{{500,500,100}, {0,0,-1}});
    CHECK(session.selection().ordered().empty());
    CHECK(doc.undoCount() == history);
}
