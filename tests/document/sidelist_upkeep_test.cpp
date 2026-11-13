// Plan 0.8 D11 rules 2-4: side lists follow the faces they name through edits.
#include "../editor/fixtures.hpp"
#include "document/overlay_fragments.hpp"
#include "document/sidelist_upkeep.hpp"
#include "editor/map_problems.hpp"

#include <algorithm>
#include <sstream>

using namespace editor_test;

namespace {

const char* kEmpty = "versioninfo\n{\n\"editorversion\" \"400\"\n}\nworld\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n}\n";

NodeHandle world(const Document& doc) {
    for (const auto r : doc.roots())
        if (doc.get(*doc.handle(r))->block->key() == "world") return *doc.handle(r);
    FAIL("no world");
    return {};
}

SideId faceWith(const Document& doc, NodeHandle solid, DVec3 normal) {
    const auto poly = geometry::solidGeometry(*doc.get(solid)->block);
    REQUIRE(poly);
    for (const auto& face : poly->poly.faces)
        if (glm::dot(face.plane.normal, normal) > 0.999) return static_cast<SideId>(face.sideId);
    FAIL("no such face");
    return 0;
}

std::string overlayText(std::string_view classname, DVec3 origin, double halfU, double halfV, std::string_view sides,
                        std::string_view extra = "") {
    std::ostringstream text;
    text << "entity\n{\n\"id\" \"0\"\n\"classname\" \"" << classname << "\"\n\"origin\" \"" << origin.x << ' ' << origin.y
         << ' ' << origin.z << "\"\n\"BasisOrigin\" \"" << origin.x << ' ' << origin.y << ' ' << origin.z
         << "\"\n\"BasisU\" \"1 0 0\"\n\"BasisV\" \"0 1 0\"\n\"BasisNormal\" \"0 0 1\"\n"
         << "\"uv0\" \"" << -halfU << ' ' << -halfV << " 0\"\n\"uv1\" \"" << -halfU << ' ' << halfV << " 0\"\n"
         << "\"uv2\" \"" << halfU << ' ' << halfV << " 0\"\n\"uv3\" \"" << halfU << ' ' << -halfV << " 0\"\n"
         << "\"material\" \"overlays/test\"\n\"sides\" \"" << sides << "\"\n" << extra << "}\n";
    return text.str();
}

std::vector<int> listOf(const Document& doc, NodeHandle entity, std::string_view key = "sides") {
    const auto* value = doc.get(entity)->block->find(key);
    REQUIRE(value);
    auto sides = geometry::parseSideList(value->decodedValue().value_or(""));
    std::ranges::sort(sides);
    return sides;
}

// Splits `solid` at x = `at` the way the Clip tool keeps both halves: the -X half keeps the
// solid (and the IDs of faces it still has), the +X half is a new fragment.
void split(Document& doc, NodeHandle solid, double at) {
    const auto handle = std::get<Handle<Solid>>(solid);
    const auto poly = geometry::solidGeometry(*doc.get(solid)->block)->poly;
    const auto half = [&](DVec3 normal, double distance) {
        auto sides = geometry_test::rederivedSides(poly);
        for (std::size_t i = 0; i < sides.size(); ++i) {
            sides[i].sideId = poly.faces[i].sideId;
            static_cast<geometry::FaceAttributes&>(sides[i]) = poly.faces[i];
        }
        sides.push_back(geometry_test::side(normal, distance, 0));
        return geometry_test::build(sides);
    };
    const auto front = half({1, 0, 0}, at), back = half({-1, 0, 0}, -at);
    geometry::FaceCorrespondence keep{std::in_place};
    for (std::size_t i = 0; i < front.faces.size(); ++i)
        if (front.faces[i].sideId != 0) keep->emplace(i, front.faces[i].sideId);
    auto tx = doc.begin("clip");
    tx.createFragment(handle, back);
    tx.rebuildSolid(handle, front, keep);
    tx.commit();
}

std::vector<SideId> topFaces(const Document& doc) {
    std::vector<SideId> result;
    for (const auto& h : nodes(doc)) {
        if (!std::holds_alternative<Handle<Solid>>(h)) continue;
        result.push_back(faceWith(doc, h, {0, 0, 1}));
    }
    std::ranges::sort(result);
    return result;
}

} // namespace

TEST_CASE("[document][sidelist] rule 2: a deleted face leaves every side list in the same undo step") {
    Document doc(parsed(kEmpty));
    const auto w = world(doc);
    const auto a = addBox(doc, {0, 0, 0}, {64, 64, 64}, w);
    const auto b = addBox(doc, {128, 0, 0}, {192, 64, 64}, w);
    const auto topA = faceWith(doc, a, {0, 0, 1}), topB = faceWith(doc, b, {0, 0, 1});
    const auto sides = std::to_string(topA) + " " + std::to_string(topB);
    const auto overlay = addBlock(doc, overlayText("info_overlay", {32, 32, 64}, 16, 16, sides));
    const auto cubemap = addBlock(doc, "entity { id 0 classname env_cubemap origin \"0 0 128\" sides \"" + sides + "\" }");
    // A sidelist key only an FGD knows about.
    const auto custom = addBlock(doc, "entity { id 0 classname prop_sides origin \"0 0 0\" faces \"" + sides + "\" note \"" +
                                          sides + "\" }");
    doc.setSideListTypes([](std::string_view classname, std::string_view key) {
        return classname == "prop_sides" && key == "faces" ? InstanceKeyType::SideList : InstanceKeyType::String;
    });
    const auto history = doc.undoCount();
    {
        auto tx = doc.begin("delete");
        tx.destroy(a);
        const auto changes = tx.commit();
        CHECK(std::ranges::find(changes.vanishedSides, topA) != changes.vanishedSides.end());
    }
    CHECK(doc.undoCount() == history + 1);
    CHECK(listOf(doc, overlay) == std::vector<int>{topB});
    CHECK(listOf(doc, cubemap) == std::vector<int>{topB});
    CHECK(listOf(doc, custom, "faces") == std::vector<int>{topB});
    CHECK(doc.get(custom)->block->find("note")->decodedValue() == sides); // Not a sidelist.
    REQUIRE(doc.undo());
    CHECK(listOf(doc, overlay) == std::vector<int>{std::min(topA, topB), std::max(topA, topB)});
    CHECK(listOf(doc, custom, "faces").size() == 2);
    REQUIRE(doc.redo());
    CHECK(listOf(doc, overlay) == std::vector<int>{topB});
}

TEST_CASE("[document][sidelist] rule 3: a split face's pieces join the lists that held it, within the footprint") {
    Document doc(parsed(kEmpty));
    const auto w = world(doc);
    const auto box = addBox(doc, {-64, -64, -64}, {64, 64, 0}, w);
    const auto top = faceWith(doc, box, {0, 0, 1});
    const auto side = faceWith(doc, box, {0, -1, 0});
    // One overlay straddles the cut at x = 0, one sits wholly on the -X half, one on a wall.
    const auto across = addBlock(doc, overlayText("info_overlay", {0, 0, 0}, 32, 32, std::to_string(top)));
    const auto leftOnly = addBlock(doc, overlayText("info_overlay", {-40, 0, 0}, 16, 16, std::to_string(top)));
    const auto cubemap = addBlock(doc, "entity { id 0 classname env_cubemap origin \"0 0 64\" sides \"" +
                                           std::to_string(top) + " " + std::to_string(side) + "\" }");
    split(doc, box, 0.0);
    const auto tops = topFaces(doc);
    REQUIRE(tops.size() == 2);
    CHECK(std::ranges::find(tops, top) != tops.end()); // The kept half keeps the ID.
    CHECK(listOf(doc, across) == std::vector<int>(tops.begin(), tops.end()));
    CHECK(listOf(doc, leftOnly) == std::vector<int>{top});
    // A cubemap has no footprint: it takes every coplanar piece, but not the new cap faces.
    const auto cubemapList = listOf(doc, cubemap);
    CHECK(cubemapList.size() == 4);
    for (const auto t : tops) CHECK(std::ranges::find(cubemapList, t) != cubemapList.end());
    for (const auto id : cubemapList) CHECK(doc.side(id));
    // Undo restores the lists with the faces.
    REQUIRE(doc.undo());
    CHECK(listOf(doc, across) == std::vector<int>{top});
    CHECK(listOf(doc, cubemap).size() == 2);
}

TEST_CASE("[document][sidelist] splitting a face under an overlay keeps the overlay on both halves") {
    Document doc(parsed(kEmpty));
    const auto w = world(doc);
    const auto box = addBox(doc, {-64, -64, -64}, {64, 64, 0}, w);
    const auto top = faceWith(doc, box, {0, 0, 1});
    const auto overlay = addBlock(doc, overlayText("info_overlay", {8, 0, 0}, 32, 24, std::to_string(top)));
    const auto before = overlayGeometry(doc, *doc.get(overlay)->block);
    REQUIRE(before.pieces.size() == 1);
    const double area = geometry::fragmentArea(before.pieces[0].fragment);
    CHECK(area == doctest::Approx(64.0 * 48.0));

    split(doc, box, 0.0);
    const auto after = overlayGeometry(doc, *doc.get(overlay)->block);
    CHECK(after.missing.empty());
    REQUIRE(after.pieces.size() == 2);
    CHECK(after.pieces[0].solid != after.pieces[1].solid);
    double total = 0.0;
    for (const auto& piece : after.pieces) total += geometry::fragmentArea(piece.fragment);
    CHECK(total == doctest::Approx(area));
    // The +X half carries the larger share (the overlay is centred at x = 8).
    const auto share = [&](std::size_t i) { return geometry::fragmentArea(after.pieces[i].fragment); };
    CHECK(std::max(share(0), share(1)) == doctest::Approx(40.0 * 48.0));
}

TEST_CASE("[document][sidelist] rule 4: an overlay left on no face is kept and flagged in Problems") {
    Document doc(parsed(kEmpty));
    const auto w = world(doc);
    const auto box = addBox(doc, {0, 0, 0}, {64, 64, 64}, w);
    const auto top = faceWith(doc, box, {0, 0, 1});
    const auto overlay = addBlock(doc, overlayText("info_overlay", {32, 32, 64}, 16, 16, std::to_string(top),
                                                   "\"targetname\" \"sign\"\n"));
    const auto overlayId = id(doc, overlay);
    const auto problems = [&] {
        const editor::ProblemContext context{doc};
        auto all = editor::ProblemRegistry::builtin().run(context);
        std::erase_if(all, [](const auto& p) { return p.kind != editor::ProblemKind::EmptyOverlay; });
        return all;
    };
    CHECK(problems().empty());
    {
        auto tx = doc.begin("delete");
        tx.destroy(box);
        tx.commit();
    }
    REQUIRE(doc.handle(overlayId)); // Kept.
    CHECK(listOf(doc, overlay).empty());
    const auto flagged = problems();
    REQUIRE(flagged.size() == 1);
    CHECK(flagged[0].severity == editor::ProblemSeverity::Warning);
    CHECK(flagged[0].objects == std::vector<NodeId>{overlayId});
    CHECK(flagged[0].text.find("info_overlay \"sign\"") != std::string::npos);
    REQUIRE(doc.undo());
    CHECK(problems().empty());
}
