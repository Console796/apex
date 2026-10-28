#include <doctest/doctest.h>
#include "document/cordon.hpp"
#include "fixtures.hpp"

using namespace apex;
using namespace editor_test;

namespace {
NodeHandle world(const Document& doc) { return *doc.handle(doc.roots().front()); }

std::string text(const Document& doc) { return doc.write().value(); }

// Returns the parsed filtered map's world solids (by min corner x) and entity classnames.
struct Kept {
    std::vector<double> solidMinX;
    std::vector<std::string> entities;
    int walls = 0;
};
Kept kept(const std::string& vmf) {
    auto parsedText = kv1::parse(vmf);
    REQUIRE(parsedText);
    Kept result;
    for (const auto& block : parsedText->root().children()) {
        if (block.key() == "entity") {
            result.entities.push_back(block.find("targetname") ? block.find("targetname")->decodedValue().value() : "?");
        } else if (block.key() == "world") {
            for (const auto& child : block.children()) {
                if (child.kind() != kv1::NodeKind::Block || child.key() != "solid") continue;
                const auto geometry = geometry::solidGeometry(child);
                REQUIRE(geometry);
                const auto bounds = geometry->poly.bounds();
                const auto* material = child.find("side")->find("material");
                if (material && material->decodedValue().value() == "tools/toolsskybox") ++result.walls;
                else result.solidMinX.push_back(bounds.min.x);
            }
        }
    }
    return result;
}
} // namespace

TEST_CASE("cordon read and write preserve the block and unknown keys") {
    Document doc(parsed(
        "world { id 1 classname worldspawn }\n"
        "cordons { \"active\" \"1\" \"extra\" \"keep\" cordon { \"name\" \"a\" \"active\" \"1\" \"note\" \"x\" "
        "box { \"mins\" \"(-8 -16 -24)\" \"maxs\" \"(8 16 24)\" } } "
        "cordon { \"name\" \"b\" \"active\" \"0\" box { \"mins\" \"(0 0 0)\" \"maxs\" \"(64 64 64)\" } } }"));
    auto set = document::readCordons(doc);
    REQUIRE(set.boxes.size() == 2);
    CHECK(set.enabled);
    CHECK(set.boxes[0].name == "a");
    CHECK(set.boxes[0].active);
    CHECK(set.boxes[0].box.min == DVec3{-8, -16, -24});
    CHECK(set.boxes[0].box.max == DVec3{8, 16, 24});
    CHECK_FALSE(set.boxes[1].active);
    CHECK(set.compileBoxes().size() == 1);

    document::setCordonBox(doc, set.boxes[0].id, AABB{{-32, -32, 0}, {32, 32, 128}});
    document::setCordonActive(doc, set.boxes[1].id, true);
    const auto out = text(doc);
    CHECK(out.find("\"extra\"") != std::string::npos);
    CHECK(out.find("\"note\"") != std::string::npos);
    CHECK(out.find("(-32 -32 0)") != std::string::npos);
    CHECK(document::readCordons(doc).compileBoxes().size() == 2);
    CHECK(doc.undo());
    CHECK(document::readCordons(doc).compileBoxes().size() == 1);
}

TEST_CASE("cordon without a cordons block creates one and removal restores it") {
    Document doc(parsed("world { id 1 classname worldspawn }"));
    CHECK_FALSE(document::readCordons(doc).container);
    const auto id = document::addCordon(doc, "cordon", AABB{{0, 0, 0}, {128, 128, 128}});
    auto set = document::readCordons(doc);
    REQUIRE(set.boxes.size() == 1);
    CHECK(set.boxes[0].id == id);
    CHECK(set.enabled);
    CHECK(text(doc).find("mins") != std::string::npos);
    document::removeCordon(doc, id);
    CHECK(document::readCordons(doc).boxes.empty());
}

TEST_CASE("a drag of a cordon box coalesces into one undo step") {
    Document doc(parsed("world { id 1 classname worldspawn }"));
    const auto id = document::addCordon(doc, "c", AABB{{0, 0, 0}, {64, 64, 64}});
    const auto before = doc.undoCount();
    for (int i = 1; i <= 4; ++i)
        document::setCordonBox(doc, id, AABB{{0, 0, 0}, {64.0 + 16 * i, 64, 64}}, "cordon-drag");
    CHECK(doc.undoCount() == before + 1);
    CHECK(document::readCordons(doc).boxes[0].box.max.x == 128);
}

TEST_CASE("cordon filter keeps what touches a box, adds six walls and leaves the document alone") {
    Document doc(parsed("world { id 1 classname worldspawn }"));
    addBox(doc, {0, 0, 0}, {64, 64, 64}, world(doc));          // inside
    addBox(doc, {96, 0, 0}, {160, 64, 64}, world(doc));        // straddles the +x face of the box
    addBox(doc, {1000, 0, 0}, {1064, 64, 64}, world(doc));     // outside
    addBlock(doc, "entity { id 20 classname info_target targetname inside origin \"32 32 32\" }");
    addBlock(doc, "entity { id 21 classname info_target targetname outside origin \"2000 0 0\" }");
    addBlock(doc, "entity { id 22 classname info_target targetname nowhere }");
    auto tx = doc.begin("brush entity");
    auto entity = tx.create(block("entity { id 23 classname func_detail targetname straddler }"));
    tx.create(solidBlock(geometry_test::build(geometry_test::boxSides({1500, 0, 0}, {1564, 64, 64}))), entity);
    tx.create(solidBlock(geometry_test::build(geometry_test::boxSides({100, 0, 0}, {120, 64, 64}))), entity);
    tx.commit();
    const auto before = text(doc);

    const auto filtered = document::cordonFilteredText(before, {AABB{{-16, -16, -16}, {128, 128, 128}}}, {});
    REQUIRE(filtered);
    const auto result = kept(*filtered);
    CHECK(result.solidMinX == std::vector<double>{0, 96});
    CHECK(result.walls == 6);
    CHECK(result.entities == std::vector<std::string>{"inside", "nowhere", "straddler"});
    CHECK(text(doc) == before);
}

TEST_CASE("no boxes leaves the text untouched") {
    const std::string vmf = "world { id 1 classname worldspawn }\n";
    CHECK(document::cordonFilteredText(vmf, {}, {}).value() == vmf);
}
