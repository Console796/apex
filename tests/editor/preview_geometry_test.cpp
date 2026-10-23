#include "fixtures.hpp"
#include "editor/preview.hpp"
#include "geometry/solid_geometry.hpp"
#include "render/solid_mesh.hpp"

using namespace editor_test;

TEST_CASE("preview geometry equal writes assignment and clear preserve the revision clock") {
    Preview preview;
    const auto poly = geometry_test::build(geometry_test::boxSides());
    preview.setReplacements({});
    preview.setGhosts({});
    CHECK(preview.revision() == 0);
    preview.setReplacement(42, poly);
    CHECK_FALSE(preview.empty());
    CHECK(preview.revision() == 1);
    preview.setReplacement(42, poly);
    preview.setReplacements(preview.replacements());
    CHECK(preview.revision() == 1);
    auto changed = poly;
    changed.faces[0].material = "other/material";
    preview.setReplacement(42, changed);
    CHECK(preview.revision() == 2);
    preview.setGhosts({{poly, "ghost/material"}});
    CHECK(preview.revision() == 3);
    preview.setGhosts(preview.ghosts());
    preview = preview;
    CHECK(preview.revision() == 3);
    Preview copy = preview;
    preview = copy;
    CHECK(preview.revision() == 3);
    auto ghosts = preview.ghosts();
    ghosts[0].opacity = 0.2f;
    preview.setGhosts(ghosts);
    CHECK(preview.revision() == 4);
    preview.set(42, Transform{});
    preview.clear(42); // Removes the transform and replacement in one write.
    CHECK(preview.revision() == 6);
    CHECK(preview.replacements().empty());
    CHECK(preview.transforms().empty());
    CHECK_FALSE(preview.empty());
    preview.clear(42);
    CHECK(preview.revision() == 6);
    preview = Preview{};
    CHECK(preview.empty());
    CHECK(preview.revision() == 7);
    preview.clear();
    CHECK(preview.revision() == 7);
    preview = copy;
    CHECK(preview.revision() == 8);
    preview.clear();
    CHECK(preview.empty());
    CHECK(preview.revision() == 9);
}

TEST_CASE("preview geometry Bite 6 ghost and committed value use identical mesh vertices") {
    Document doc(parsed());
    const auto handle = addBox(doc, {-13, -7, -3}, {29, 41, 53});
    auto shape = geometry::solidGeometry(*doc.get(handle)->block);
    REQUIRE(shape);
    // A non-affine edit preserving planarity: widen the top, leaving the bottom fixed.
    for (auto& point : shape->poly.vertices) point.x *= 1.0 + (point.z + 3.0) / 224.0;
    for (auto& face : shape->poly.faces) face.plane = *shape->poly.derivePlane(face);
    Preview preview;
    preview.setGhosts({{shape->poly, ""}});
    const auto size = [](std::string_view) { return render::TextureSize{128, 32}; };
    const auto ghost = render::meshSolid(preview.ghosts()[0].poly, size);
    REQUIRE(ghost);
    auto tx = doc.begin("Commit preview value");
    tx.setSolidGeometry(std::get<Handle<Solid>>(handle), *shape);
    tx.commit();
    const auto committed = render::meshSolid(*doc.get(handle)->block, size);
    REQUIRE(committed);
    CHECK(ghost->origin == committed->origin);
    REQUIRE(ghost->batches.size() == committed->batches.size());
    for (std::size_t i = 0; i < ghost->batches.size(); ++i) {
        const auto& a = ghost->batches[i];
        const auto& b = committed->batches[i];
        CHECK(a.material == b.material);
        REQUIRE(a.vertices.size() == b.vertices.size());
        for (const auto& vertex : a.vertices) {
            CHECK(std::ranges::any_of(b.vertices, [&](const auto& other) {
                for (int j = 0; j < 3; ++j)
                    if (vertex.position[j] != other.position[j] || vertex.normal[j] != other.normal[j]) return false;
                return vertex.uv[0] == other.uv[0] && vertex.uv[1] == other.uv[1] && vertex.material == other.material;
            }));
        }
    }
}

TEST_CASE("preview geometry Bite 7 never changes picking document or history") {
    Document doc(parsed());
    const auto handle = addBox(doc, {0, -16, -16}, {32, 16, 16});
    PickIndex picks(doc);
    picks.attach(doc);
    const Ray ray{{-128, 0, 0}, {1, 0, 0}};
    const auto before = picks.first(ray);
    REQUIRE(before);
    const auto bytes = doc.write().value();
    const auto history = doc.undoCount();
    int notifications = 0;
    doc.observe([&](const ChangeSet&) { ++notifications; });
    Preview preview;
    const auto ghost = geometry_test::build(geometry_test::boxSides({-64, -8, -8}, {-32, 8, 8}));
    preview.setGhosts({{ghost, "test"}});
    preview.setReplacement(id(doc, handle), ghost);
    REQUIRE(picks.first(ray));
    CHECK(picks.first(ray)->node == before->node);
    CHECK(picks.first(ray)->point == before->point);
    preview.clear();
    CHECK(doc.write().value() == bytes);
    CHECK(doc.undoCount() == history);
    CHECK(notifications == 0);
}
