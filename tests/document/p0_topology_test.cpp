// 0.3.1 P0 red tests (plan 0.3 D2, D3). Committed failing; 0.3.1 turns them green.
#include "../editor/fixtures.hpp"
#include "editor/editor_session.hpp"
#include "geometry/face_matching.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace editor_test;

namespace {
vmf::VmfFile fixture(std::string_view name) {
    const auto path = std::filesystem::path(APEX_FIXTURE_DIR) / "topology" / name;
    std::ifstream input(path, std::ios::binary);
    REQUIRE_MESSAGE(input.good(), path.string());
    return parsed(std::string{std::istreambuf_iterator<char>(input), {}});
}

Handle<Solid> solid(const Document& doc) {
    for (const auto handle : nodes(doc)) {
        if (const auto* result = std::get_if<Handle<Solid>>(&handle)) return *result;
    }
    FAIL("fixture has no solid");
    return {};
}
} // namespace

TEST_CASE("P0 red: diagonal cube clip writes a new cap face in place (D2)") {
    Document doc(fixture("p0_cube.vmf"));
    const auto handle = solid(doc);
    auto geometry = geometry::solidGeometry(*doc.get(handle)->block);
    REQUIRE(geometry);
    REQUIRE(geometry->poly.faces.size() == 6);
    auto planes = geometry_test::rederivedSides(geometry->poly);
    for (std::size_t i = 0; i < planes.size(); ++i) {
        static_cast<geometry::FaceAttributes&>(planes[i]) = geometry->poly.faces[i];
    }
    planes.push_back(geometry_test::side(glm::normalize(DVec3{1, 1, 1}), 96.0, 0));
    geometry->poly = geometry_test::build(planes);
    geometry->sides = geometry_test::pointData(geometry->poly);
    REQUIRE(geometry->poly.faces.size() == 7);
    {
        auto tx = doc.begin("clip cube");
        CHECK_NOTHROW(tx.setSolidGeometry(handle, *geometry));
        tx.commit();
    }
    const auto written = geometry::solidGeometry(*doc.get(handle)->block);
    REQUIRE(written);
    CHECK(written->poly.faces.size() == 7);
    CHECK(doc.get(handle)->block->findAll("side").size() == 7);
    CHECK(written->poly.validate().empty());
    const auto bytes = doc.write();
    REQUIRE(bytes);
    CHECK(vmf::parse(*bytes).has_value());
}

TEST_CASE("P0 red: session move accepts a redundant source side (D2)") {
    Document doc(fixture("p0_redundant_side.vmf"));
    const auto handle = solid(doc);
    const auto before = geometry::solidGeometry(*doc.get(handle)->block);
    REQUIRE(before);
    REQUIRE(before->sides.size() == 7);
    REQUIRE(before->poly.faces.size() == 6);
    EditorSession session;
    session.attach(doc);
    session.perform("selection.mode.solids");
    session.selectNode(doc.get(handle)->id);
    REQUIRE(session.gizmoContext());
    const auto history = doc.undoCount();
    const DVec3 delta{16, 32, 48};
    CHECK(session.moveSelection(delta, true));
    CHECK(doc.undoCount() == history + 1);
    const auto after = geometry::solidGeometry(*doc.get(handle)->block);
    REQUIRE(after);
    checkPoint(after->poly.bounds().min, before->poly.bounds().min + delta);
    checkPoint(after->poly.bounds().max, before->poly.bounds().max + delta);
}

TEST_CASE("P0 red: moved cube vertex keeps adjacent face identities (D3)") {
    Document doc(fixture("p0_cube.vmf"));
    const auto geometry = geometry::solidGeometry(*doc.get(solid(doc))->block);
    REQUIRE(geometry);
    const auto& original = geometry->poly;
    const DVec3 corner{64, 64, 64};
    REQUIRE(std::ranges::any_of(original.vertices, [&](auto point) {
        return glm::length(point - corner) < 1e-6;
    }));
    const auto moved = corner - glm::normalize(DVec3{1, 1, 1}) * 16.0;
    auto planes = geometry_test::rederivedSides(original);
    std::vector<std::size_t> adjacent;
    for (std::size_t i = 0; i < original.faces.size(); ++i) {
        const auto& face = original.faces[i];
        const auto vertex = std::ranges::find_if(face.vertices, [&](auto index) {
            return glm::length(original.vertices[index] - corner) < 1e-6;
        });
        if (vertex == face.vertices.end()) continue;
        adjacent.push_back(i);
        const auto offset = static_cast<std::size_t>(vertex - face.vertices.begin());
        const auto previous = original.vertices[face.vertices[(offset + face.vertices.size() - 1) % face.vertices.size()]];
        const auto next = original.vertices[face.vertices[(offset + 1) % face.vertices.size()]];
        // No hull builder yet: define the three tilted planes through the moved corner.
        planes[i].plane = Plane::fromPoints(previous, moved, next);
    }
    REQUIRE(adjacent.size() == 3);
    auto rebuilt = geometry_test::build(planes);
    REQUIRE(rebuilt.faces.size() == 6);
    REQUIRE(std::ranges::any_of(rebuilt.vertices, [&](auto point) {
        return glm::length(point - moved) < 1e-6;
    }));
    // Plane matching loses every tilted face, which is why D3 needs explicit correspondence.
    // 0.3.1 turns this green by rebuilding through rebuildSolid with a FaceCorrespondence.
    const auto matches = geometry::matchFaces(original, rebuilt);
    CHECK(matches.matched.size() == 6);
    for (std::size_t i = 0; i < original.faces.size(); ++i) {
        CAPTURE(original.faces[i].sideId);
        CAPTURE(std::ranges::find(adjacent, i) != adjacent.end());
        CHECK(std::ranges::any_of(matches.matched, [&](const auto& match) {
            return match.oldIndex == i && rebuilt.faces[match.newIndex].sideId == original.faces[i].sideId;
        }));
    }
}
