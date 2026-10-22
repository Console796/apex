#include "fixtures.hpp"
#include "editor/preview.hpp"
#include "render/solid_mesh.hpp"
#include "render/view_math.hpp"

#include <array>

using namespace editor_test;

TEST_CASE("preview revision changes only when its value changes") {
    Preview preview;
    CHECK(preview.empty());
    CHECK(preview.revision() == 0);
    preview.clear();
    preview.clear(42);
    CHECK(preview.revision() == 0);
    const auto move = Transform::translate({1, 2, 3});
    preview.set(42, move);
    CHECK(preview.revision() == 1);
    preview.set(42, move);
    preview.set(preview.transforms());
    preview.clear(99);
    CHECK(preview.revision() == 1);
    preview.set(43, Transform{});
    CHECK(preview.revision() == 2);
    preview.clear(42);
    CHECK(preview.revision() == 3);
    preview.clear();
    CHECK(preview.revision() == 4);
    CHECK(preview.empty());
    preview.clear();
    CHECK(preview.revision() == 4);
    const std::array<NodeId, 3> ids{1, 2, 1};
    const auto from = fromTransform(ids, move);
    CHECK(from.transforms().size() == 2);
    CHECK(from.transforms().at(1) == move.matrix());
    CHECK(from.transforms().at(2) == move.matrix());
    CHECK(from.revision() == 1);
    CHECK(fromTransform({}, move).revision() == 0);
    preview.set(from.transforms());
    CHECK(preview.revision() == 5);
    preview.set(from.transforms());
    CHECK(preview.revision() == 5);
    preview = fromTransform(ids, Transform::translate({4, 5, 6}));
    CHECK(preview.revision() == 6);
    preview = fromTransform(ids, Transform::translate({4, 5, 6}));
    CHECK(preview.revision() == 6);
    preview = Preview{};
    CHECK(preview.empty());
    CHECK(preview.revision() == 7);
}

TEST_CASE("preview cancel leaves document history observers and picking untouched") {
    Document doc(parsed());
    const auto solid = addBox(doc, {0, 0, 0}, {32, 32, 32});
    const auto node = id(doc, solid);
    PickIndex picks(doc);
    picks.attach(doc);
    const auto bytes = doc.write().value();
    const auto history = doc.undoCount();
    int notifications = 0;
    doc.observe([&](const ChangeSet&) { ++notifications; });
    const Ray ray{{-64, 16, 16}, {1, 0, 0}};
    const auto hit = picks.first(ray);
    REQUIRE(hit);
    Preview preview;
    for (int i = 0; i < 100; ++i) preview.set(node, Transform::translate({double(i), 0, 0}));
    preview.clear();
    CHECK(doc.write().value() == bytes);
    CHECK(doc.undoCount() == history);
    CHECK(notifications == 0);
    REQUIRE(picks.first(ray));
    CHECK(picks.first(ray)->node == hit->node);
    CHECK(picks.first(ray)->point == hit->point);
}

TEST_CASE("preview Bite 9 packed GPU positions and normals equal transformed and committed geometry") {
    for (const double base : {0.0, 65536.0, -65536.0}) {
        const DVec3 pivot{base + 3, -base - 9, base + 2};
        const auto frame = Transform::rotate({1, 2, 3}, 37).linear();
        const std::array transforms{
            Transform::translate({12.5, -7, 3}),
            Transform::rotate({1, 2, -1}, 43, pivot),
            Transform::scale({2, 0.5, 1.25}, frame, pivot),
            Transform::mirror({1, 2, 3}, pivot)};
        for (std::size_t t = 0; t < transforms.size(); ++t) {
            CAPTURE(base);
            CAPTURE(t);
            const auto& transform = transforms[t];
            Document doc(parsed());
            const auto handle = addBox(doc, pivot + DVec3(-13, -5, 3), pivot + DVec3(27, 43, 35));
            const auto solid = geometry::solidGeometry(*doc.get(handle)->block);
            REQUIRE(solid);
            const auto mesh = render::meshSolid(*doc.get(handle)->block, [](std::string_view) { return render::TextureSize{}; });
            REQUIRE(mesh);
            const DVec3 camera = transform.point(mesh->origin) + DVec3(128, -64, 32);
            const auto packed = render::cameraRelativePreview(transform.matrix(), mesh->origin, camera);
            const auto expected = transformedVertices(*solid, transform);
            const std::array selection{id(doc, handle)};
            auto preview = fromTransform(selection, transform);
            REQUIRE(commitTransform(doc, selection, transform, {}, "Transform"));
            preview.clear(); // Commit and clear in the same tick.
            CHECK(preview.empty());
            const auto committed = geometry::solidGeometry(*doc.get(handle)->block);
            REQUIRE(committed);
            for (const auto& batch : mesh->batches) {
                for (const auto& vertex : batch.vertices) {
                    const auto position = render::previewPosition(packed, {vertex.position[0], vertex.position[1], vertex.position[2]});
                    const auto matches = [&](DVec3 p) { return glm::length(DVec3(position) - (p - camera)) < 3e-5; };
                    // Includes input and output float rounding; below a 1/32768 world unit.
                    CHECK(std::ranges::any_of(expected, matches));
                    CHECK(std::ranges::any_of(committed->poly.vertices, matches));
                    const Vec3 oldNormal{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
                    const auto normal = render::previewNormal(packed, oldNormal);
                    const auto expectedNormal = glm::normalize(glm::transpose(glm::inverse(transform.linear())) * DVec3(oldNormal));
                    CHECK(glm::length(DVec3(normal) - expectedNormal) < 2e-7);
                }
            }
        }
    }
}

TEST_CASE("preview view_math rebases before narrowing at huge coordinates") {
    const DVec3 origin{1e12, -2e12, 3e12};
    const auto transform = Transform::scale({2, 0.5, -1}, DMat3(1), origin);
    const DVec3 camera = origin + DVec3(0.25, -0.5, 1);
    const auto packed = render::cameraRelativePreview(transform.matrix(), origin, camera);
    CHECK(render::previewPosition(packed, {0.125f, 0.25f, -0.5f}) == Vec3(0, 0.625f, -0.5f));
}
