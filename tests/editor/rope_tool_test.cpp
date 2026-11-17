#include "face_fixtures.hpp"
#include "editor/rope_tool.hpp"

using namespace face_test;

namespace {
InputEvent ropeKey(std::string key) {
    InputEvent event;
    event.kind = InputKind::KeyDown;
    event.key = std::move(key);
    return event;
}
std::string ropeValue(const kv1::Node& block, std::string_view key) {
    const auto* value = block.find(key);
    return value ? value->decodedValue().value_or("") : "";
}
}

TEST_CASE("Rope tool creates a snapped linked chain with collision-free names in one undo step") {
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) {
        Scene s;
        s.session.setPreset(*preset);
        if (preset->name == "Hammer") {
            assets::EntityClassTable classes;
            for (const auto* classname : {"move_rope", "keyframe_rope"}) {
                assets::EntityClass cls;
                cls.name = classname;
                cls.type = "MoveClass";
                for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
                         {"Width", "6"}, {"RopeMaterial", "cable/custom"}, {"NextKey", "must_not_leak"}}) {
                    assets::EntityProperty property;
                    property.name = key;
                    property.defaultValue = value;
                    cls.properties.push_back(std::move(property));
                }
                classes.classes[classname] = std::move(cls);
            }
            s.session.setEntityClasses(std::move(classes));
        }
        auto occupied = kv1::Node::block("entity");
        occupied.append(kv1::Node::value("classname", "info_target")).value();
        occupied.append(kv1::Node::value("targetname", "ROPE_1")).value();
        auto tx = s.doc.begin("Existing name");
        tx.create(std::move(occupied));
        tx.commit();
        REQUIRE(s.session.perform("tool.rope"));
        const auto before = s.doc.write().value();
        const auto undo = s.doc.undoCount();
        std::vector<DVec3> expected;
        for (const auto normal : {DVec3{1, 0, 0}, DVec3{0, 1, 0}, DVec3{0, 0, 1}}) {
            const auto v = view(Scene::ray(normal));
            expected.push_back(s.session.snap(v.cursorRay, v).point);
            REQUIRE(s.session.input(click(), v));
        }
        CHECK(s.doc.undoCount() == undo);
        REQUIRE(s.session.activeToolObject().overlay({}, 1).worldPoints.size() == 3);
        REQUIRE(s.session.input(ropeKey("Enter"), {}));
        CHECK(s.doc.undoCount() == undo + 1);
        CHECK(s.session.activeToolObject().overlay({}, 1).worldPoints.empty());
        for (std::size_t i = 0; i < 3; ++i) {
            const auto name = "rope_" + std::to_string(i + 2);
            const auto ids = s.session.names().find(name);
            REQUIRE(ids.size() == 1);
            const auto& block = *s.doc.get(*s.doc.handle(ids[0]))->block;
            CHECK(ropeValue(block, "classname") == (i == 0 ? "move_rope" : "keyframe_rope"));
            CHECK(ropeValue(block, "NextKey") == (i < 2 ? "rope_" + std::to_string(i + 3) : ""));
            const auto origin = block.find("origin")->asVec3();
            REQUIRE(origin);
            CHECK(DVec3{(*origin)[0], (*origin)[1], (*origin)[2]} == expected[i]);
            CHECK(ropeValue(block, "Width") == (preset->name == "Hammer" ? "6" : "2"));
            CHECK(ropeValue(block, "RopeMaterial") == (preset->name == "Hammer" ? "cable/custom" : "cable/cable"));
        }
        const auto after = s.doc.write().value();
        REQUIRE(s.session.perform("edit.undo"));
        CHECK(s.doc.write().value() == before);
        REQUIRE(s.session.perform("edit.redo"));
        CHECK(s.doc.write().value() == after);
    }
}

TEST_CASE("Rope tool cancels drafts and rejects a single point without creating undo entries") {
    Scene s;
    REQUIRE(s.session.perform("tool.rope"));
    const auto before = s.doc.write().value();
    const auto undo = s.doc.undoCount();
    const auto v = view(Scene::ray({1, 0, 0}));
    REQUIRE(s.session.input(click(), v));
    REQUIRE(s.session.input(click(), v)); // Duplicate clicks do not add zero-length spans.
    CHECK(s.session.activeToolObject().overlay({}, 1).worldPoints.size() == 1);
    REQUIRE(s.session.input(ropeKey("Enter"), {}));
    CHECK(s.doc.undoCount() == undo);
    REQUIRE(s.session.input(ropeKey("Backspace"), {}));
    CHECK(s.session.activeToolObject().overlay({}, 1).worldPoints.empty());
    REQUIRE(s.session.input(click(), v));
    REQUIRE(s.session.input(ropeKey("Escape"), {}));
    CHECK(s.session.activeToolObject().overlay({}, 1).worldPoints.empty());
    REQUIRE(s.session.input(click(), v));
    REQUIRE(s.session.perform("tool.select"));
    REQUIRE(s.session.perform("tool.rope"));
    CHECK(s.session.activeToolObject().overlay({}, 1).worldPoints.empty());
    CHECK(s.doc.undoCount() == undo);
    CHECK(s.doc.write().value() == before);
}
