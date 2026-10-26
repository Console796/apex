// 0.3.1 topology write regressions (plan 0.3 D2, D3).
#include "../editor/fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/vertex_edit.hpp"
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

TEST_CASE("P0: diagonal cube clip writes a new cap face in place (D2)") {
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
        CHECK_NOTHROW(tx.rebuildSolid(handle, geometry->poly));
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

TEST_CASE("P0: session move accepts a redundant source side (D2)") {
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

TEST_CASE("P0: moved cube vertex keeps all six face identities (D3)") {
    Document doc(fixture("p0_cube.vmf"));
    const auto handle = solid(doc);
    const auto source = geometry::solidGeometry(*doc.get(handle)->block);
    REQUIRE(source);
    REQUIRE(source->poly.faces.size() == 6);
    const DVec3 corner{64, 64, 64};
    const auto vertex = std::ranges::find_if(source->poly.vertices, [&](DVec3 point) {
        return glm::length(point-corner) < 1e-6;
    });
    REQUIRE(vertex != source->poly.vertices.end());
    const auto index = static_cast<std::size_t>(vertex-source->poly.vertices.begin());
    const auto result = editor::evaluateVertexEdit(source->poly, std::span{&index, 1},
        editor::Transform::translate(-glm::normalize(DVec3{1,1,1})*16.0));
    REQUIRE(result);
    const auto moved = corner - glm::normalize(DVec3{1,1,1})*16.0;
    CHECK(std::ranges::any_of(result->poly.vertices, [&](DVec3 point) {
        return glm::length(point-moved) < 1e-6;
    }));
    auto tx = doc.begin("move vertex");
    tx.rebuildSolid(handle, result->poly, result->correspondence);
    CHECK(tx.commit().vanishedSides.empty());
    const auto written = geometry::solidGeometry(*doc.get(handle)->block);
    REQUIRE(written);
    for (const auto& oldFace : source->poly.faces) {
        CAPTURE(oldFace.sideId);
        const auto face = std::ranges::find(written->poly.faces, oldFace.sideId,
                                            &geometry::Face::sideId);
        REQUIRE(face != written->poly.faces.end());
    }
}
