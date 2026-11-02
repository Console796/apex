#include "render/lightmap_atlas.hpp"
#include "render/solid_mesh.hpp"
#include "geometry/solid_block.hpp"
#include "../geometry/fixtures.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::render;

TEST_CASE("lightmap atlas: shelves are deterministic, guarded and nonoverlapping") {
    std::vector<lighting::BakeFace> faces(80);
    for (std::size_t i = 0; i < faces.size(); ++i) {
        faces[i].chart.dimensions = {1 + int(i % 13), 1 + int(i % 17)};
        faces[i].solidId = i / 2;
        faces[i].sideId = 7;
    }
    auto a = LightmapAtlas::pack(faces, 256);
    auto b = LightmapAtlas::pack(faces, 256);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->size == b->size);
    CHECK(a->rects == b->rects);
    CHECK(a->fragments.at({0, 7}) == std::vector<std::size_t>{0, 1});
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const auto r = a->rects[i];
        CHECK(r.x >= 1);
        CHECK(r.y >= 1);
        CHECK(r.x + r.width < a->size);
        CHECK(r.y + r.height < a->size);
        for (std::size_t j = 0; j < i; ++j) {
            const auto q = a->rects[j];
            CHECK((r.x + r.width + 1 <= q.x - 1 || q.x + q.width + 1 <= r.x - 1 ||
                   r.y + r.height + 1 <= q.y - 1 || q.y + q.height + 1 <= r.y - 1));
        }
    }
    CHECK_FALSE(LightmapAtlas::pack(faces, 16));
    faces[0].chart.dimensions = {0, 1};
    CHECK_FALSE(LightmapAtlas::pack(faces));
    faces[0].chart.dimensions = {4095, 1};
    CHECK_FALSE(LightmapAtlas::pack(faces));
    CHECK_FALSE(LightmapAtlas::pack({}, 0));
    CHECK(LightmapAtlas::pack({})->size == 1);
    faces.resize(1);
    faces[0].chart.dimensions = {14, 14};
    CHECK(LightmapAtlas::pack(faces, 16)->size == 16);
    CHECK_FALSE(LightmapAtlas::pack(faces, 15));
}

TEST_CASE("lightmap atlas: chart sample centres map to texel centres") {
    std::vector<DVec3> vertices{{-1, -3, 8}, {63, -3, 72}, {63, 61, 72}, {-1, 61, 8}};
    geometry::Face face;
    face.vertices = {0, 1, 2, 3};
    face.plane = {glm::normalize(DVec3{-1, 0, 1}), 9 / std::sqrt(2.0)};
    face.uv.uaxis = {1, 1, 0};
    face.uv.vaxis = {0, -1, 0};
    face.uv.shiftU = 8;
    face.uv.scaleU = -0.5;
    auto faces = lighting::bakeFaces(face, vertices);
    auto atlas = LightmapAtlas::pack(faces);
    REQUIRE(atlas);
    const auto& r = atlas->rects[0];
    for (int t = 0; t < r.height; ++t) for (int s = 0; s < r.width; ++s) {
        const auto uv = atlas->uv(0, faces[0].chart.worldAt({s, t}));
        CHECK(uv.x * atlas->size == doctest::Approx(r.x + s + 0.5));
        CHECK(uv.y * atlas->size == doctest::Approx(r.y + t + 0.5));
    }
}

TEST_CASE("lightmap atlas: meshing preserves source identity across chart fragments") {
    auto planes = geometry_test::boxSides({-32, -32, -32}, {4096, 32, 32});
    for (auto& side : planes) {
        side.material = "test/one";
        side.lightmapScale = 16;
        side.uv.uaxis = std::abs(side.plane.normal.x) > 0.5 ? DVec3{0, 1, 0} : DVec3{1, 0, 0};
        side.uv.vaxis = std::abs(side.plane.normal.z) > 0.5 ? DVec3{0, 1, 0} : DVec3{0, 0, 1};
    }
    const auto poly = geometry_test::build(planes);
    std::vector<lighting::BakeFace> faces;
    for (const auto& face : poly.faces) {
        auto fragments = lighting::bakeFaces(face, poly.vertices);
        for (auto& fragment : fragments) { fragment.solidId = 42; faces.push_back(std::move(fragment)); }
    }
    REQUIRE(faces.size() > poly.faces.size());
    auto atlas = LightmapAtlas::pack(faces);
    REQUIRE(atlas);
    const auto size = [](std::string_view) { return TextureSize{64, 64}; };
    auto mesh = meshSolid(poly, size, &*atlas, 42);
    REQUIRE(mesh);
    REQUIRE(mesh->batches.size() == 1);
    const auto& batch = mesh->batches[0];
    CHECK(batch.faces.size() == poly.faces.size());
    std::size_t vertex = 0, fragment = 0;
    for (const auto& source : poly.faces) {
        const auto& indices = atlas->fragments.at({42, source.sideId});
        for (const auto i : indices) for (const auto point : faces[i].vertices) {
            REQUIRE(vertex < batch.vertices.size());
            const auto& v = batch.vertices[vertex++];
            const auto uv = atlas->uv(i, point);
            CHECK(v.lightmapUv[0] == doctest::Approx(uv.x));
            CHECK(v.lightmapUv[1] == doctest::Approx(uv.y));
            CHECK(v.position[0] + mesh->origin.x == doctest::Approx(point.x));
        }
        vertex += source.vertices.size(); // Original edge perimeter, with no internal fragment edges.
        fragment += indices.size();
    }
    CHECK(fragment == faces.size());
    CHECK(vertex == batch.vertices.size());
    CHECK(batch.edges.size() == 48);
    auto unrelated = meshSolid(poly, size, &*atlas, 43);
    REQUIRE(unrelated);
    CHECK(unrelated->batches[0].vertices.size() == 24);
    for (const auto& v : unrelated->batches[0].vertices) CHECK(v.lightmapUv[0] < 0.0f);
}

TEST_CASE("lightmap atlas: stale layout leaves edited brush geometry intact") {
    auto planes = geometry_test::boxSides();
    auto poly = geometry_test::build(planes);
    std::vector<lighting::BakeFace> faces;
    // Only one side is charted: the rest must remain unmapped.
    faces = lighting::bakeFaces(poly.faces[0], poly.vertices);
    for (auto& face : faces) face.solidId = 42;
    auto atlas = LightmapAtlas::pack(faces);
    REQUIRE(atlas);
    for (auto& vertex : poly.vertices) vertex += DVec3{32, 16, 8};
    for (auto& face : poly.faces) face.plane.dist += glm::dot(face.plane.normal, DVec3{32, 16, 8});
    auto mesh = meshSolid(poly, [](std::string_view) { return TextureSize{}; }, &*atlas, 42);
    REQUIRE(mesh);
    CHECK(mesh->origin == DVec3(32, 16, 8));
    for (const auto& batch : mesh->batches) for (const auto& vertex : batch.vertices)
        CHECK(vertex.lightmapUv[0] < 0.0f);

    SUBCASE("live retention samples old radiance but always uses current geometry") {
        atlas->retainStale = true;
        auto retained = meshSolid(poly, [](std::string_view) { return TextureSize{}; }, &*atlas, 42);
        REQUIRE(retained);
        CHECK(retained->origin == mesh->origin);
        const auto& rect = atlas->rects[0];
        unsigned mapped = 0;
        for (const auto& batch : retained->batches) for (const auto& vertex : batch.vertices) {
            if (vertex.lightmapUv[0] < 0) continue;
            ++mapped;
            CHECK(vertex.lightmapUv[0] >= float((rect.x + 0.5) / atlas->size));
            CHECK(vertex.lightmapUv[0] <= float((rect.x + rect.width - 0.5) / atlas->size));
            CHECK(vertex.lightmapUv[1] >= float((rect.y + 0.5) / atlas->size));
            CHECK(vertex.lightmapUv[1] <= float((rect.y + rect.height - 0.5) / atlas->size));
        }
        CHECK(mapped == poly.faces[0].vertices.size());
        REQUIRE(retained->batches.size() == mesh->batches.size());
        for (std::size_t b = 0; b < mesh->batches.size(); ++b) {
            REQUIRE(retained->batches[b].vertices.size() == mesh->batches[b].vertices.size());
            for (std::size_t v = 0; v < mesh->batches[b].vertices.size(); ++v)
                for (int axis = 0; axis < 3; ++axis)
                    CHECK(retained->batches[b].vertices[v].position[axis] == mesh->batches[b].vertices[v].position[axis]);
        }
    }
}

TEST_CASE("lightmap atlas: displacement sides retain their separate shading flag") {
    auto block = geometry::solidBlock(geometry_test::build(geometry_test::boxSides()), {"test/one"});
    auto sides = block.findAll("side");
    for (std::size_t i = 0; i < sides.size(); ++i)
        sides[i]->find("id")->setValue(std::to_string(i + 1)).value();
    sides[4]->append(kv1::Node::block("dispinfo")).value();
    auto mesh = meshSolid(block, [](std::string_view) { return TextureSize{}; });
    REQUIRE(mesh);
    REQUIRE(mesh->batches[0].faces.size() == 6);
    for (const auto& face : mesh->batches[0].faces) CHECK(face.displacement == (face.side == 5));
}

TEST_CASE("lightmap atlas: restart retains same-side radiance with resized charts") {
    lighting::BakeFace face;
    face.solidId = 42;
    face.sideId = 7;
    face.chart.dimensions = {2, 1};
    face.chart.worldToLuxel = {DVec3{1, 0, 0}, DVec3{0, 1, 0}};
    face.chart.luxelToWorld = face.chart.worldToLuxel;
    auto previous = LightmapAtlas::pack(std::array{face});
    REQUIRE(previous);
    face.chart.dimensions = {3, 1};
    auto added = face;
    added.sideId = 8;
    auto current = LightmapAtlas::pack(std::array{face, added});
    REQUIRE(current);
    const std::vector<std::vector<Vec3>> radiance{{Vec3{1}, Vec3{2}}};
    const auto retained = current->retainRadiance(&*previous, radiance);
    CHECK(retained[0] == std::vector<Vec3>{Vec3{1}, Vec3{2}, Vec3{2}});
    CHECK(retained[1] == std::vector<Vec3>(3, Vec3{0}));
    CHECK(current->retainRadiance(nullptr, {}) == std::vector<std::vector<Vec3>>(2, std::vector<Vec3>(3, Vec3{0})));
}
