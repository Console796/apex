#include "document/document.hpp"
#include "geometry/face_uv.hpp"
#include "geometry/tolerances.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using namespace apex;
using namespace apex::document;
namespace fs = std::filesystem;

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_MESSAGE(input.good(), path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}
vmf::VmfFile parse(const std::string& bytes) {
    auto file = vmf::parse(bytes);
    if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
    REQUIRE(file);
    return std::move(*file);
}
std::vector<Handle<Solid>> solids(const Document& doc) {
    std::vector<Handle<Solid>> result;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto handle = *doc.handle(id);
        if (const auto* solid = std::get_if<Handle<Solid>>(&handle)) result.push_back(*solid);
        for (const auto child : doc.get(handle)->children) self(self, child);
    };
    for (const auto id : doc.roots()) walk(walk, id);
    return result;
}
std::string saved(const Document& doc) {
    const auto bytes = doc.write();
    REQUIRE(bytes);
    return *bytes;
}
std::string spelling(const kv1::Node& node, const std::string& bytes) {
    const auto span = node.sourceSpan();
    return bytes.substr(span.offset, span.length);
}
void sameVertices(const geometry::ConvexPolyhedron& a, const geometry::ConvexPolyhedron& b) {
    REQUIRE(a.vertices.size() == b.vertices.size());
    for (const auto point : a.vertices) {
        CHECK(std::any_of(b.vertices.begin(), b.vertices.end(), [&](auto other) {
            return glm::length(point - other) <= geometry::kPointDataTrustEps;
        }));
    }
}

void identity(const fs::path& path, const std::set<std::string>& invalid,
              std::set<std::string>& rejected, std::size_t& count) {
    const auto bytes = read(path);
    Document doc(parse(bytes));
    const auto before = saved(doc);
    REQUIRE(before == bytes);
    auto tx = doc.begin("identity geometry");
    for (const auto handle : solids(doc)) {
        ++count;
        const auto& block = *doc.get(handle)->block;
        const auto key = path.generic_string() + "\t" + block.find("id")->decodedValue().value();
        CAPTURE(key);
        auto geometry = geometry::solidGeometry(block);
        if (!geometry) {
            CHECK_MESSAGE(invalid.contains(key), geometry.error().what());
            rejected.insert(key);
            CHECK_THROWS_AS(tx.setSolidGeometry(handle, {}), std::invalid_argument);
            continue;
        }
        tx.setSolidGeometry(handle, *geometry);
    }
    CHECK(tx.commit().empty());
    CHECK(saved(doc) == before);
    CHECK(doc.dirtyNodes().empty());
    CHECK(doc.undoCount() == 0);
}
} // namespace

TEST_CASE("setSolidGeometry identity preserves every fixture byte") {
    const auto root = fs::path(APEX_FIXTURE_DIR) / "vmf";
    const std::set<std::string> invalid{
        (root / "everything_lf.vmf").generic_string() + "\t2",
        (root / "everything_lf.vmf").generic_string() + "\t5"};
    std::set<std::string> rejected;
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.path().extension() != ".vmf") continue;
        CAPTURE(entry.path().string());
        identity(entry.path(), invalid, rejected, count);
    }
    CHECK(count == 7);
    CHECK(rejected == invalid);
}

TEST_CASE("setSolidGeometry identity preserves every P2CE corpus byte") {
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping geometry write corpus");
        return;
    }
    const auto root = fs::path(install) / "sdk_content/maps";
    REQUIRE(fs::is_directory(root));
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    REQUIRE_FALSE(files.empty());
    std::set<std::string> invalid, rejected;
    std::istringstream inventory(read(fs::path(APEX_FIXTURE_DIR) / "geometry/expected_invalid.txt"));
    for (std::string line; std::getline(inventory, line);) {
        const auto reason = line.find("\treconciled:");
        if (reason != std::string::npos && line.find(";planes:", reason) != std::string::npos)
            invalid.insert(root.generic_string() + "/" + line.substr(0, reason));
    }
    std::size_t count = 0;
    for (const auto& path : files) {
        CAPTURE(path.string());
        identity(path, invalid, rejected, count);
    }
    CHECK(count > 0);
    CHECK(rejected == invalid);
    MESSAGE("Geometry identity corpus: ", files.size(), " VMFs, ", count,
            " solids; ", rejected.size(), " inventoried invalid solids retained unchanged");
}

TEST_CASE("setSolidGeometry matches IDs, round trips transformed points and undoes exact bytes") {
    auto bytes = read(fs::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf");
    bytes.insert(bytes.find("\t\t\t\"plane\""),
                 "\t\t\t\"unknown_key\" \"keep this spelling\"\n"
                 "\t\t\tdispinfo { power 2 unknown { key value } }\n");
    for (const bool rotated : {false, true}) {
        auto file = parse(bytes);
        Document doc(file);
        const auto handle = solids(doc)[0];
        auto geometry = geometry::solidGeometry(*doc.get(handle)->block).value();
        const DMat3 rotation = rotated ? DMat3{{std::cos(0.37), std::sin(0.37), 0},
                                             {-std::sin(0.37), std::cos(0.37), 0}, {0, 0, 1}} :
                                        DMat3{1.0};
        for (auto& point : geometry.poly.vertices)
            point = rotation * point + DVec3{12345.678901234567, -789.1234567890123, 0.123456789012345};
        for (auto& face : geometry.poly.faces) {
            face.plane = geometry.poly.derivePlane(face).value();
            face.uv.uaxis = rotation * face.uv.uaxis;
            face.uv.vaxis = rotation * face.uv.vaxis;
            face.material = "must not be written";
            face.lightmapScale = 99;
            face.smoothingGroups = 99;
        }
        std::reverse(geometry.poly.faces.begin(), geometry.poly.faces.end());
        std::reverse(geometry.sides.begin(), geometry.sides.end());
        auto tx = doc.begin("transform brush");
        tx.setSolidGeometry(handle, geometry);
        const auto changes = tx.commit();
        REQUIRE(changes.modified.size() == 1);
        CHECK(changes.modified[0].flags == Dirty::Geometry);
        CHECK(changes.vanishedSides.empty());
        const auto output = saved(doc);
        auto reloaded = parse(output);
        const auto& solid = reloaded.world->solids[0];
        auto actual = geometry::solidGeometry(*solid.node);
        REQUIRE(actual);
        sameVertices(actual->poly, geometry.poly);
        REQUIRE(solid.sides.size() == file.world->solids[0].sides.size());
        for (std::size_t i = 0; i < solid.sides.size(); ++i) {
            const auto& side = solid.sides[i];
            const auto& old = file.world->solids[0].sides[i];
            CHECK(side.id()->text().value() == old.id()->text().value());
            REQUIRE(side.pointData);
            auto points = side.pointData->points().value();
            CHECK(side.pointData->find("numpts")->asInt() == static_cast<int>(points.size()));
            const auto plane = side.plane()->asPlane().value();
            const auto vec = [](auto p) { return DVec3{p[0], p[1], p[2]}; };
            const auto derived = geometry::planePointsFromSource(vec(plane.points[0]), vec(plane.points[1]),
                                                                 vec(plane.points[2])).value();
            for (const auto& point : points)
                CHECK(std::abs(derived.signedDistance(vec(point))) <= geometry::kPointDataTrustEps);
            for (const auto& point : plane.points) {
                CHECK(std::any_of(points.begin(), points.end(), [&](auto vertex) { return vertex == point; }));
            }
            // Texture lock rewrites the axes, and `rotation` follows them (D15).
            for (const auto& child : old.node->children()) {
                if (child.key() == "plane" || child.key() == "point_data" ||
                    child.key() == "uaxis" || child.key() == "vaxis" || child.key() == "rotation") continue;
                REQUIRE(side.node->find(child.key()));
                CHECK(spelling(child, bytes) == spelling(*side.node->find(child.key()), output));
            }
            const auto& face = *std::ranges::find(actual->poly.faces, static_cast<std::uint32_t>(side.id()->asInt().value()),
                                                  &geometry::Face::sideId);
            CHECK(side.node->find("rotation")->asDouble().value() ==
                  doctest::Approx(geometry::deriveRotation(face.uv, face.plane.normal)).epsilon(1e-5));
        }
        const auto oldSpan = file.world->solids[0].node->sourceSpan();
        const auto newSpan = solid.node->sourceSpan();
        CHECK(output.substr(0, newSpan.offset) == bytes.substr(0, oldSpan.offset));
        CHECK(output.substr(newSpan.offset + newSpan.length) == bytes.substr(oldSpan.offset + oldSpan.length));
        CHECK(doc.undo());
        CHECK(saved(doc) == bytes);
        CHECK(doc.redo());
        CHECK(saved(doc) == output);
    }
}

TEST_CASE("setSolidGeometry leaves untouched sides and geometry bytes alone on a UV edit") {
    const auto bytes = read(fs::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf");
    auto file = parse(bytes);
    Document doc(file);
    const auto handle = solids(doc)[0];
    auto geometry = geometry::solidGeometry(*doc.get(handle)->block).value();
    geometry.poly.faces[2].uv.uaxis = {0.123456789012345, 1.1e-7, -0.987654321098765};
    auto tx = doc.begin("change one UV axis");
    tx.setSolidGeometry(handle, geometry);
    const auto changes = tx.commit();
    REQUIRE(changes.modified.size() == 1);
    CHECK(changes.modified[0].flags == Dirty::Geometry);
    const auto output = saved(doc);
    auto reloaded = parse(output);
    for (std::size_t i = 0; i < geometry.poly.faces.size(); ++i) {
        const auto& old = file.world->solids[0].sides[i];
        const auto& side = reloaded.world->solids[0].sides[i];
        if (i != 2) CHECK(spelling(*side.node, output) == spelling(*old.node, bytes));
        else {
            // `rotation` follows the written axes (D15); every other key keeps its bytes.
            for (const auto& child : old.node->children()) {
                if (child.key() != "uaxis" && child.key() != "rotation")
                    CHECK(spelling(*side.node->find(child.key()), output) == spelling(child, bytes));
            }
            const auto& face = geometry.poly.faces[2];
            CHECK(side.node->find("rotation")->asDouble().value() ==
                  doctest::Approx(geometry::deriveRotation(face.uv, face.plane.normal)).epsilon(1e-5));
            CHECK(side.uaxis()->asUvAxis()->axis == vmf::Vec3{0.123456789012345, 1.1e-7, -0.987654321098765});
        }
    }
    CHECK(doc.undo());
    CHECK(saved(doc) == bytes);
}

TEST_CASE("setSolidGeometry rejects mismatched and duplicate side IDs before changing bytes") {
    const auto bytes = read(fs::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf");
    for (int mismatch = 0; mismatch < 6; ++mismatch) {
        Document doc(parse(bytes));
        const auto handle = solids(doc)[0];
        auto geometry = geometry::solidGeometry(*doc.get(handle)->block).value();
        switch (mismatch) {
        case 0: geometry.sides[0].sideId = 99; break;
        case 1: geometry.poly.faces[0].sideId = 99; break;
        case 2: geometry.sides.pop_back(); break;
        case 3: geometry.poly.faces.pop_back(); break;
        case 4: geometry.sides[0].sideId = geometry.sides[1].sideId; break;
        case 5: geometry.poly.faces[0].sideId = geometry.poly.faces[1].sideId; break;
        }
        auto tx = doc.begin("invalid geometry");
        CHECK_THROWS_AS(tx.setSolidGeometry(handle, geometry), std::invalid_argument);
        CHECK(tx.commit().empty());
        CHECK(saved(doc) == bytes);
        CHECK(doc.undoCount() == 0);
    }
}

TEST_CASE("setSolidGeometry adds Strata point_data only when a plane-only brush changes") {
    const auto bytes = read(fs::path(APEX_FIXTURE_DIR) / "vmf/render_map.vmf");
    Document doc(parse(bytes));
    const auto handle = solids(doc)[0];
    auto geometry = geometry::solidGeometry(*doc.get(handle)->block).value();
    for (const auto& side : geometry.sides) REQUIRE(side.points.empty());
    for (auto& point : geometry.poly.vertices) point += DVec3{0.123456789012345, 13.5, -27.25};
    for (auto& face : geometry.poly.faces) face.plane = geometry.poly.derivePlane(face).value();
    auto tx = doc.begin("translate plane-only brush");
    tx.setSolidGeometry(handle, geometry);
    tx.commit();
    auto file = parse(saved(doc));
    auto actual = geometry::solidGeometry(*file.world->solids[0].node);
    REQUIRE(actual);
    for (const auto& side : actual->sides) CHECK_FALSE(side.points.empty());
    sameVertices(actual->poly, geometry.poly);
    CHECK(doc.undo());
    CHECK(saved(doc) == bytes);
}

TEST_CASE("setSolidGeometry retains redundant sides on identity and drops them on transforms") {
    auto file = parse(read(fs::path(APEX_FIXTURE_DIR) / "vmf/render_map.vmf"));
    auto& solid = file.world->solids[0];
    auto duplicate = *solid.sides[0].node;
    REQUIRE(duplicate.find("id")->setValue("99999"));
    REQUIRE(solid.node->append(std::move(duplicate)));
    Document doc(file);
    const auto bytes = saved(doc);
    const auto handle = solids(doc)[0];
    auto geometry = geometry::solidGeometry(*doc.get(handle)->block).value();
    REQUIRE(geometry.sides.size() == geometry.poly.faces.size() + 1);
    auto tx = doc.begin("redundant side identity");
    tx.setSolidGeometry(handle, geometry);
    CHECK(tx.commit().empty());
    CHECK(saved(doc) == bytes);
    for (auto& point : geometry.poly.vertices) point += DVec3{300, 0, 0};
    for (auto& face : geometry.poly.faces) face.plane = geometry.poly.derivePlane(face).value();
    // D2: a transform goes through the rebuild path, so the redundant side vanishes.
    auto move = doc.begin("redundant side transform");
    move.setSolidGeometry(handle, geometry);
    const auto changes = move.commit();
    CHECK(changes.vanishedSides.size() == 1);
    CHECK(doc.get(handle)->block->findAll("side").size() == geometry.poly.faces.size());
    for (const auto& face : geometry.poly.faces) CHECK(doc.side(static_cast<SideId>(face.sideId)));
    REQUIRE(doc.undo());
    CHECK(saved(doc) == bytes);
}

TEST_CASE("setSolidGeometry rewrites fallback geometry with consistent point_data") {
    auto file = parse(read(fs::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf"));
    auto values = file.world->solids[0].sides[0].pointData->findAll("point");
    REQUIRE(values.size() >= 2);
    const auto first = values[0]->text().value();
    REQUIRE(values[1]->set("1" + first.substr(first.find(' '))));
    Document doc(file);
    const auto original = saved(doc);
    const auto handle = solids(doc)[0];
    auto geometry = geometry::solidGeometry(*doc.get(handle)->block).value();
    REQUIRE(geometry.usedPlaneFallback);
    for (auto& point : geometry.poly.vertices) point += DVec3{128.25, -64.5, 3.125};
    for (auto& face : geometry.poly.faces) face.plane = geometry.poly.derivePlane(face).value();
    auto tx = doc.begin("translate fallback brush");
    tx.setSolidGeometry(handle, geometry);
    REQUIRE(tx.commit().modified.size() == 1);
    auto written = parse(saved(doc));
    const auto rebuilt = geometry::solidGeometry(*written.world->solids[0].node);
    REQUIRE(rebuilt);
    CHECK_FALSE(rebuilt->usedPlaneFallback);
    for (const auto& side : rebuilt->sides) CHECK_FALSE(side.points.empty());
    sameVertices(rebuilt->poly, geometry.poly);
    CHECK(doc.undo());
    CHECK(saved(doc) == original);
}

TEST_CASE("setSolidGeometry rejects non-finite UV shift without changing the document") {
    const auto bytes = read(fs::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf");
    Document doc(parse(bytes));
    const auto handle = solids(doc)[0];
    auto geometry = geometry::solidGeometry(*doc.get(handle)->block).value();
    geometry.poly.faces[0].uv.shiftU = std::numeric_limits<double>::quiet_NaN();
    auto tx = doc.begin("invalid UV shift");
    CHECK_THROWS_AS(tx.setSolidGeometry(handle, geometry), std::invalid_argument);
    CHECK(tx.commit().empty());
    CHECK(saved(doc) == bytes);
    CHECK(doc.dirtyNodes().empty());
    CHECK(doc.undoCount() == 0);
}
