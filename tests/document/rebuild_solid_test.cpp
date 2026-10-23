#include "../editor/fixtures.hpp"
#include "editor/editor_session.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

using namespace editor_test;
namespace fs = std::filesystem;

namespace {
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_MESSAGE(input.good(), path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}
std::string saved(const Document& doc) {
    auto bytes = doc.write();
    REQUIRE(bytes);
    return *bytes;
}
std::vector<Handle<Solid>> solids(const Document& doc) {
    std::vector<Handle<Solid>> result;
    for (const auto& handle : nodes(doc))
        if (const auto* solid = std::get_if<Handle<Solid>>(&handle)) result.push_back(*solid);
    return result;
}
geometry::FaceCorrespondence identity(const geometry::ConvexPolyhedron& poly) {
    geometry::FaceCorrespondence result{std::in_place};
    for (std::size_t n = 0; n < poly.faces.size(); ++n) result->emplace(n, poly.faces[n].sideId);
    return result;
}
void identityFile(const fs::path& path, std::size_t& count, std::size_t& invalid) {
    auto bytes = read(path);
    auto file = vmf::parse(bytes);
    if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
    REQUIRE(file);
    Document doc(*file);
    const auto before = saved(doc);
    REQUIRE(before == bytes);
    auto tx = doc.begin("identity rebuild");
    for (const auto handle : solids(doc)) {
        CAPTURE(path.string());
        const auto poly = geometry::solidGeometry(*doc.get(handle)->block);
        if (!poly) {
            ++invalid;
            CHECK_THROWS_AS(tx.rebuildSolid(handle, {}), std::invalid_argument);
            continue;
        }
        ++count;
        tx.rebuildSolid(handle, poly->poly, identity(poly->poly));
    }
    CHECK(tx.commit().empty());
    CHECK(saved(doc) == before);
    CHECK(doc.dirtyNodes().empty());
    CHECK(doc.undoCount() == 0);
}
Document cube() {
    auto bytes = read(fs::path(APEX_FIXTURE_DIR) / "topology/p0_cube.vmf");
    // Keep non-side data and matched-side payloads with unusual spelling and duplicate keys.
    auto file = parsed(bytes);
    auto* solid = file.world->solids[0].node;
    REQUIRE(solid->append(kv1::Node::value("custom_solid", "untouched")));
    REQUIRE(solid->append(block("editor { color \"12 34 56\" comments \"keep membership block\" }")));
    for (auto* side : solid->findAll("side")) {
        const auto id = side->find("id")->decodedValue().value();
        REQUIRE(side->find("material")->setValue("brick/face_" + id));
        REQUIRE(side->find("uaxis")->setValue("[1 0 0 " + id + "] 0.25"));
        REQUIRE(side->find("vaxis")->setValue("[0 -1 0 -" + id + "] -0.5"));
        REQUIRE(side->append(block("dispinfo { power 2 unknown { key value } }")));
        REQUIRE(side->append(kv1::Node::value("unknown_side", "keep")));
        REQUIRE(side->append(kv1::Node::value("unknown_side", "also keep")));
    }
    return Document(file);
}
geometry::ConvexPolyhedron clipped(const geometry::ConvexPolyhedron& poly,
                                   DVec3 normal = glm::normalize(DVec3{1, 1, 1}),
                                   double distance = 96.0) {
    auto planes = geometry_test::rederivedSides(poly);
    // Caller IDs and attributes are deliberately wrong; matching and inheritance own them.
    for (auto& plane : planes) plane.sideId = poly.faces[0].sideId;
    planes.push_back(geometry_test::side(normal, distance, poly.faces[0].sideId));
    return geometry_test::build(planes);
}
const kv1::Node& side(const Document& doc, SideId id) {
    const auto location = doc.side(id);
    REQUIRE(location);
    return *doc.get(location->solid)->block->findAll("side")[location->faceIndex];
}
std::vector<std::string> keys(const kv1::Node& block) {
    std::vector<std::string> result;
    for (const auto& child : block.children()) result.emplace_back(child.key());
    return result;
}
} // namespace

TEST_CASE("rebuildSolid identity preserves every fixture solid byte") {
    std::size_t count = 0, invalid = 0;
    for (const auto& entry : fs::recursive_directory_iterator(APEX_FIXTURE_DIR)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".vmf") continue;
        identityFile(entry.path(), count, invalid);
    }
    CHECK(count > 0);
    MESSAGE("Identity rebuild fixtures: ", count, " valid solids; ", invalid, " invalid retained");
}

TEST_CASE("rebuildSolid identity preserves every P2CE corpus solid byte") {
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("APEX_P2CE_DIR unset; skipping rebuild corpus"); return; }
    const auto root = fs::path(install) / "sdk_content/maps";
    REQUIRE(fs::is_directory(root));
    std::size_t files = 0, count = 0, invalid = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".vmf") continue;
        ++files;
        identityFile(entry.path(), count, invalid);
    }
    CHECK(files > 0);
    CHECK(count > 0);
    CHECK(invalid == 0);
    MESSAGE("Identity rebuild corpus: ", files, " VMFs, ", count, " solids");
}

TEST_CASE("rebuildSolid cap inherits Hammer ordered keys and replaces colliding caller IDs") {
    auto doc = cube();
    const auto handle = solids(doc)[0];
    const auto nodeId = doc.get(handle)->id;
    const auto original = *doc.get(handle)->block;
    const auto before = saved(doc);
    const auto oldPoly = geometry::solidGeometry(original)->poly;
    const auto poly = clipped(oldPoly);
    REQUIRE(poly.faces.size() == 7);
    auto matched = poly;
    const auto matches = geometry::matchFaces(oldPoly, matched);
    REQUIRE(matches.unmatchedNew.size() == 1);
    const auto parent = oldPoly.faces[*matches.unmatchedNew[0].inheritFrom].sideId;
    auto tx = doc.begin("diagonal clip");
    tx.rebuildSolid(handle, poly);
    const auto changes = tx.commit();
    CHECK(changes.created.empty());
    CHECK(changes.destroyed.empty());
    CHECK(changes.vanishedSides.empty());
    REQUIRE(changes.modified.size() == 1);
    CHECK(changes.modified[0].flags == Dirty::Geometry);
    CHECK(doc.get(handle)->id == nodeId);
    const auto* after = doc.get(handle)->block.get();
    CHECK(after->find("id")->decodedValue() == original.find("id")->decodedValue().value());
    CHECK(after->find("custom_solid")->decodedValue() == "untouched");
    CHECK(keys(*after->find("editor")) == keys(*original.find("editor")));
    for (const auto& child : original.find("editor")->children())
        CHECK(after->find("editor")->find(child.key())->rawValue() == child.rawValue());
    const auto rebuilt = geometry::solidGeometry(*after);
    REQUIRE(rebuilt);
    CHECK_FALSE(rebuilt->usedPlaneFallback);
    geometry_test::sameVertices(rebuilt->poly, poly);
    std::set<std::uint32_t> ids;
    for (const auto& face : oldPoly.faces) {
        ids.insert(face.sideId);
        const auto& written = side(doc, static_cast<SideId>(face.sideId));
        CHECK(written.find("dispinfo"));
        CHECK(written.findAll("unknown_side").size() == 2);
        const auto sourceSides = original.findAll("side");
        const auto old = std::ranges::find_if(sourceSides, [&](const auto* candidate) {
            return candidate->find("id")->asInt() == static_cast<int>(face.sideId);
        });
        REQUIRE(old != sourceSides.end());
        for (const auto key : {"material", "uaxis", "vaxis", "rotation", "lightmapscale", "smoothing_groups"})
            CHECK(written.find(key)->rawValue() == (*old)->find(key)->rawValue());
    }
    const auto cap = std::ranges::find_if(rebuilt->poly.faces, [&](const auto& face) {
        return !ids.contains(face.sideId);
    });
    REQUIRE(cap != rebuilt->poly.faces.end());
    CHECK(cap->sideId > *ids.rbegin());
    const auto& capBlock = side(doc, static_cast<SideId>(cap->sideId));
    CHECK(keys(capBlock) == std::vector<std::string>{"id", "plane", "point_data", "material", "uaxis", "vaxis",
          "rotation", "lightmapscale", "smoothing_groups"});
    for (const auto key : {"material", "uaxis", "vaxis", "rotation", "lightmapscale", "smoothing_groups"})
        CHECK(capBlock.find(key)->decodedValue() == side(doc, static_cast<SideId>(parent)).find(key)->decodedValue().value());
    CHECK_FALSE(capBlock.find("dispinfo"));
    CHECK_FALSE(capBlock.find("unknown_side"));
    const auto afterBytes = saved(doc);
    Document reloaded(parsed(afterBytes));
    CHECK(keys(side(reloaded, static_cast<SideId>(cap->sideId))) == keys(capBlock));
    REQUIRE(doc.undo());
    CHECK(saved(doc) == before);
    CHECK_FALSE(doc.side(static_cast<SideId>(cap->sideId)));
    REQUIRE(doc.redo());
    CHECK(saved(doc) == afterBytes);
    CHECK(doc.side(static_cast<SideId>(cap->sideId)));
}

TEST_CASE("rebuildSolid supplied empty correspondence replaces every side with fresh IDs") {
    auto doc = cube();
    const auto handle = solids(doc)[0];
    const auto poly = geometry::solidGeometry(*doc.get(handle)->block)->poly;
    const auto before = saved(doc);
    auto tx = doc.begin("replace all faces");
    tx.rebuildSolid(handle, poly, geometry::FaceCorrespondence{std::in_place});
    const auto changes = tx.commit();
    CHECK(changes.vanishedSides.size() == poly.faces.size());
    CHECK(doc.get(handle)->block->children()[0].key() == "id");
    for (const auto& face : poly.faces) CHECK_FALSE(doc.side(static_cast<SideId>(face.sideId)));
    for (const auto* block : doc.get(handle)->block->findAll("side")) {
        CHECK_FALSE(block->find("dispinfo"));
        CHECK_FALSE(block->find("unknown_side"));
    }
    REQUIRE(doc.undo());
    CHECK(saved(doc) == before);
}

TEST_CASE("rebuildSolid vanished sides are indexed immediately and selection prunes them") {
    auto doc = cube();
    const auto handle = solids(doc)[0];
    const auto oldPoly = geometry::solidGeometry(*doc.get(handle)->block)->poly;
    const auto poly = clipped(oldPoly, {1, 0, 0}, 0);
    auto copy = poly;
    const auto matches = geometry::matchFaces(oldPoly, copy);
    REQUIRE(matches.vanishedOld.size() == 1);
    const auto vanished = static_cast<SideId>(oldPoly.faces[matches.vanishedOld[0]].sideId);
    EditorSession session;
    session.attach(doc);
    session.selection().setMode(SelectionMode::Faces);
    session.selection().select(vanished);
    const auto before = saved(doc);
    auto tx = doc.begin("clip away face");
    tx.rebuildSolid(handle, poly);
    CHECK_FALSE(doc.side(vanished));
    const auto changes = tx.commit();
    CHECK(changes.vanishedSides == std::vector<SideId>{vanished});
    CHECK(session.selection().ordered().empty());
    const auto after = saved(doc);
    REQUIRE(doc.undo());
    CHECK(doc.side(vanished));
    CHECK(saved(doc) == before);
    REQUIRE(doc.redo());
    CHECK_FALSE(doc.side(vanished));
    CHECK(saved(doc) == after);
}

TEST_CASE("rebuildSolid explicit split parents give the largest child identity and siblings inheritance") {
    auto doc = cube();
    const auto handle = solids(doc)[0];
    const auto oldPoly = geometry::solidGeometry(*doc.get(handle)->block)->poly;
    const auto poly = clipped(oldPoly);
    auto correspondence = identity(oldPoly);
    const auto parent = oldPoly.faces[0].sideId;
    correspondence->emplace(6, parent);
    // Ensure the largest child is visited after the other child.
    auto reversed = poly;
    std::swap(reversed.faces[0], reversed.faces[6]);
    auto tx = doc.begin("split face");
    tx.rebuildSolid(handle, reversed, correspondence);
    tx.commit();
    const auto result = geometry::solidGeometry(*doc.get(handle)->block);
    REQUIRE(result);
    const auto largest = poly.faceArea(poly.faces[0]) >= poly.faceArea(poly.faces[6]) ? 0U : 6U;
    const auto owner = std::ranges::find(result->poly.faces, parent, &geometry::Face::sideId);
    REQUIRE(owner != result->poly.faces.end());
    CHECK(glm::length(owner->plane.normal - poly.faces[largest].plane.normal) < 1e-6);
    for (const auto& face : result->poly.faces) {
        if (face.sideId == parent || face.sideId == oldPoly.faces[1].sideId ||
            face.sideId == oldPoly.faces[2].sideId || face.sideId == oldPoly.faces[3].sideId ||
            face.sideId == oldPoly.faces[4].sideId || face.sideId == oldPoly.faces[5].sideId) continue;
        const auto& child = side(doc, static_cast<SideId>(face.sideId));
        CHECK(child.find("material")->decodedValue() == side(doc, static_cast<SideId>(parent)).find("material")->decodedValue().value());
        CHECK(child.find("uaxis")->decodedValue() == side(doc, static_cast<SideId>(parent)).find("uaxis")->decodedValue().value());
        CHECK_FALSE(child.find("dispinfo"));
        CHECK_FALSE(child.find("unknown_side"));
    }
}

TEST_CASE("createFragment preserves group and two visgroups with fresh solid and side IDs") {
    auto doc = cube();
    const auto source = solids(doc)[0];
    int group = 0, firstVisgroup = 0, secondVisgroup = 0;
    {
        auto tx = doc.begin("memberships");
        const auto groupHandle = tx.create(block("group { id 31 }"));
        const auto firstHandle = tx.create(block("visgroup { visgroupid 41 name first }"));
        const auto secondHandle = tx.create(block("visgroup { visgroupid 42 name second }"));
        group = doc.get(groupHandle)->block->find("id")->asInt().value();
        firstVisgroup = doc.get(firstHandle)->block->find("visgroupid")->asInt().value();
        secondVisgroup = doc.get(secondHandle)->block->find("visgroupid")->asInt().value();
        tx.setEditor(source, EditorEdit::setGroup(group));
        tx.setEditor(source, EditorEdit::addVisgroup(firstVisgroup));
        tx.setEditor(source, EditorEdit::addVisgroup(secondVisgroup));
        tx.setEditor(source, EditorEdit::setComments("fragment membership"));
        tx.commit();
    }
    const auto original = *doc.get(source)->block;
    const auto oldPoly = geometry::solidGeometry(original)->poly;
    const auto poly = clipped(oldPoly, {1, 0, 0}, 0);
    const auto before = saved(doc);
    auto tx = doc.begin("fragment");
    const auto fragment = tx.createFragment(source, poly);
    const auto changes = tx.commit();
    CHECK(changes.created.size() == 1);
    CHECK(changes.vanishedSides.empty());
    CHECK(doc.get(fragment)->parent == doc.get(source)->parent);
    CHECK(doc.get(fragment)->block->find("id")->asInt() != original.find("id")->asInt().value());
    const auto* editor = doc.get(fragment)->block->find("editor");
    REQUIRE(editor);
    CHECK(editor->find("groupid")->asInt() == group);
    const auto memberships = editor->findAll("visgroupid");
    REQUIRE(memberships.size() == 2);
    CHECK(memberships[0]->asInt() == firstVisgroup);
    CHECK(memberships[1]->asInt() == secondVisgroup);
    CHECK(editor->find("comments")->decodedValue() == "fragment membership");
    const auto result = geometry::solidGeometry(*doc.get(fragment)->block);
    REQUIRE(result);
    geometry_test::sameVertices(result->poly, poly);
    auto copy = poly;
    const auto matches = geometry::matchFaces(oldPoly, copy);
    for (const auto& face : result->poly.faces) {
        CHECK(std::ranges::none_of(oldPoly.faces, [&](const auto& old) { return old.sideId == face.sideId; }));
        CHECK(doc.side(static_cast<SideId>(face.sideId))->solid == fragment);
    }
    const auto blocks = doc.get(fragment)->block->findAll("side");
    for (const auto& match : matches.matched) {
        CHECK(blocks[match.newIndex]->find("dispinfo"));
        CHECK(blocks[match.newIndex]->findAll("unknown_side").size() == 2);
    }
    for (const auto& face : matches.unmatchedNew) {
        CHECK_FALSE(blocks[face.newIndex]->find("dispinfo"));
        CHECK_FALSE(blocks[face.newIndex]->find("unknown_side"));
    }
    const auto fragmentId = doc.get(fragment)->id;
    const auto after = saved(doc);
    REQUIRE(doc.undo());
    CHECK(saved(doc) == before);
    CHECK_FALSE(doc.handle(fragmentId));
    REQUIRE(doc.redo());
    CHECK(saved(doc) == after);
}

TEST_CASE("rebuildSolid invalid geometry and correspondence leave bytes and history unchanged") {
    auto doc = cube();
    const auto handle = solids(doc)[0];
    const auto poly = geometry::solidGeometry(*doc.get(handle)->block)->poly;
    const auto before = saved(doc);
    auto tx = doc.begin("invalid rebuilds");
    auto correspondence = identity(poly);
    correspondence->emplace(poly.faces.size(), poly.faces[0].sideId);
    CHECK_THROWS_AS(tx.rebuildSolid(handle, poly, correspondence), std::invalid_argument);
    correspondence = identity(poly);
    (*correspondence)[0] = std::numeric_limits<std::uint32_t>::max();
    CHECK_THROWS_AS(tx.rebuildSolid(handle, poly, correspondence), std::invalid_argument);
    CHECK_THROWS_AS(tx.rebuildSolid(handle, {}), std::invalid_argument);
    CHECK(tx.commit().empty());
    CHECK(saved(doc) == before);
    CHECK(doc.undoCount() == 0);
}
