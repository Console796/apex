// Apex-authored vertex edit geometry tests (MIT)
#include "fixtures.hpp"
#include "editor/vertex_edit.hpp"
#include "geometry/face_matching.hpp"
#include "geometry/solid_block.hpp"

#include <algorithm>

using namespace editor_test;

namespace {
geometry::ConvexPolyhedron cube() {
    auto poly = geometry_test::build(geometry_test::boxSides({-64,-64,-64}, {64,64,64}));
    for (std::size_t i = 0; i < poly.faces.size(); ++i) poly.faces[i].sideId = static_cast<std::uint32_t>(100+i);
    return poly;
}
std::size_t vertexAt(const geometry::ConvexPolyhedron& poly, DVec3 point) {
    const auto it = std::ranges::find_if(poly.vertices, [&](DVec3 p) { return glm::length(p-point) < 1e-8; });
    REQUIRE(it != poly.vertices.end());
    return static_cast<std::size_t>(it-poly.vertices.begin());
}
}

TEST_CASE("vertex edit evaluator preserves source faces across a 16 unit corner move") {
    const auto source = cube();
    const auto corner = vertexAt(source, {64,64,64});
    const auto result = evaluateVertexEdit(source, std::span{&corner, 1},
        Transform::translate(-glm::normalize(DVec3{1,1,1})*16.0));
    REQUIRE(result);
    CHECK(result->poly.isValid());
    CHECK(result->poly.faces.size() >= 6);
    for (const auto& face : source.faces)
        CHECK(result->correspondence->end() != std::ranges::find_if(*result->correspondence,
            [&](const auto& entry) { return entry.second == face.sideId; }));
}

TEST_CASE("vertex edit split gives the old face to its larger child") {
    const auto source = cube();
    const auto corner = vertexAt(source, {64,64,64});
    const auto originalTop = std::ranges::find_if(source.faces, [](const auto& face) {
        return face.plane.normal.z > 0.99;
    });
    REQUIRE(originalTop != source.faces.end());
    const auto result = evaluateVertexEdit(source, std::span{&corner, 1}, Transform::translate({16,0,16}));
    REQUIRE(result);
    std::vector<std::size_t> children;
    for (std::size_t i = 0; i < result->poly.faces.size(); ++i)
        if (result->correspondence->contains(i) && result->correspondence->at(i) == originalTop->sideId)
            children.push_back(i);
    REQUIRE(children.size() == 2);
    CHECK(result->poly.faces[children[0]].vertices.size() == 3);
    CHECK(result->poly.faces[children[1]].vertices.size() == 3);
    const auto owner = std::ranges::max_element(children, {}, [&](auto i) { return result->poly.faceArea(result->poly.faces[i]); });
    CHECK(result->poly.faces[*owner].sideId == originalTop->sideId);
    CHECK(std::ranges::find(result->splitFaces, *owner) == result->splitFaces.end());
    const auto sibling = children[0] == *owner ? children[1] : children[0];
    CHECK(std::ranges::find(result->splitFaces, sibling) != result->splitFaces.end());
}

TEST_CASE("vertex edit split commits the old SideId to the larger triangle") {
    const auto authored = cube();
    Document doc{parsed()};
    auto create = doc.begin("Apex-authored vertex split test solid (MIT)");
    const auto handle = std::get<Handle<Solid>>(create.create(geometry::solidBlock(authored, {"fixture"})));
    create.commit();
    const auto source = geometry::solidGeometry(*doc.get(handle)->block);
    REQUIRE(source);
    const auto corner = vertexAt(source->poly, {64,64,64});
    const auto top = std::ranges::find_if(source->poly.faces,
        [](const auto& face) { return face.plane.normal.z > 0.99; });
    REQUIRE(top != source->poly.faces.end());
    const auto result = evaluateVertexEdit(source->poly, std::span{&corner,1}, Transform::translate({16,0,16}));
    REQUIRE(result);
    auto edit = doc.begin("move vertex");
    edit.rebuildSolid(handle, result->poly, result->correspondence);
    edit.commit();
    const auto written = geometry::solidGeometry(*doc.get(handle)->block);
    REQUIRE(written);
    const auto oldId = top->sideId;
    std::vector<const geometry::Face*> children;
    for (const auto& face : written->poly.faces) {
        if (face.plane.normal.z > 0.99) children.push_back(&face);
    }
    REQUIRE(children.size() == 2);
    const auto owner = std::ranges::max_element(children, {}, [&](const auto* face) {
        return written->poly.faceArea(*face);
    });
    CHECK((*owner)->sideId == oldId);
    CHECK(std::ranges::count_if(children, [&](const auto* face) { return face->sideId == oldId; }) == 1);
}

TEST_CASE("vertex edit evaluator reports absorbed and welded vertices") {
    const auto source = cube();
    const auto corner = vertexAt(source, {64,64,64});
    const auto result = evaluateVertexEdit(source, std::span{&corner, 1}, Transform::translate({-64,-64,-64}));
    REQUIRE(result);
    CHECK(std::ranges::find(result->absorbedVertices, corner) != result->absorbedVertices.end());

    const std::array<std::size_t, 2> pair{corner, vertexAt(source, {64,64,-64})};
    const auto merged = evaluateVertexEdit(source, std::span{&pair[0], 1}, Transform::translate({0,0,-128}));
    REQUIRE(merged);
    CHECK(std::ranges::any_of(merged->mergedVertices, [&](const auto& group) {
        return std::ranges::find(group, pair[0]) != group.end() && std::ranges::find(group, pair[1]) != group.end();
    }));
}

TEST_CASE("vertex edit evaluator refuses a collapsed hull and invalid indices") {
    const auto source = cube();
    std::vector<std::size_t> top;
    for (std::size_t i = 0; i < source.vertices.size(); ++i)
        if (source.vertices[i].z > 0) top.push_back(i);
    CHECK_FALSE(evaluateVertexEdit(source, top, Transform::translate({0,0,-128})));
    const std::size_t invalid = source.vertices.size();
    CHECK_FALSE(evaluateVertexEdit(source, std::span{&invalid,1}, Transform{}));
}
