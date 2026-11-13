// 0.8.1 P0 (plan 0.8 D4): transforms carry a displacement. Before D4, applyToSolid moved the
// base face and left dispinfo behind, so a rotated hill was saved sculpting sideways.
#include "fixtures.hpp"
#include "document/displacement.hpp"
#include "editor/transform.hpp"
#include "geometry/displacement.hpp"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace editor_test;

namespace {

constexpr SideId kSide = 355; // The displaced top of solid 637 in test1_disp.vmf.

std::string fixture() {
    std::ifstream in(std::string(APEX_FIXTURE_DIR) + "/vmf/disp/test1_disp.vmf", std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

geometry::DisplacementMesh meshOf(const Document& doc, SideId side) {
    const auto disp = document::displacement(doc, side);
    REQUIRE(disp);
    REQUIRE(disp->has_value());
    const auto location = doc.side(side);
    REQUIRE(location);
    const auto solid = geometry::solidGeometry(*doc.get(location->solid)->block);
    REQUIRE(solid);
    const auto face = std::ranges::find(solid->poly.faces, static_cast<std::uint32_t>(side), &geometry::Face::sideId);
    REQUIRE(face != solid->poly.faces.end());
    const auto corners = geometry::displacementCorners(solid->poly, *face);
    REQUIRE(corners);
    auto mesh = geometry::displacementMesh(*corners, **disp);
    REQUIRE(mesh);
    return std::move(*mesh);
}

// The moved mesh is the original mesh moved: every vertex lands on the image of one original
// vertex with the same alpha, and triangles (with their tags) map to triangles.
void checkCarried(const geometry::DisplacementMesh& before, const geometry::DisplacementMesh& after,
                  const Transform& transform) {
    REQUIRE(after.positions.size() == before.positions.size());
    std::vector<std::uint32_t> image(before.positions.size());
    for (std::size_t i = 0; i < before.positions.size(); ++i) {
        const auto expected = transform.point(before.positions[i]);
        std::size_t best = 0;
        for (std::size_t j = 1; j < after.positions.size(); ++j)
            if (glm::distance(after.positions[j], expected) < glm::distance(after.positions[best], expected)) best = j;
        CAPTURE(i);
        REQUIRE(glm::distance(after.positions[best], expected) < 1e-6);
        CHECK(after.alphas[best] == before.alphas[i]);
        image[i] = static_cast<std::uint32_t>(best);
    }
    CHECK(std::set(image.begin(), image.end()).size() == image.size());
    std::map<std::set<std::uint32_t>, int> triangles;
    for (std::size_t t = 0; t < after.triangles.size(); ++t)
        triangles[{after.triangles[t].begin(), after.triangles[t].end()}] = after.tags[t];
    for (std::size_t t = 0; t < before.triangles.size(); ++t) {
        const auto& tri = before.triangles[t];
        const auto found = triangles.find({image[tri[0]], image[tri[1]], image[tri[2]]});
        CAPTURE(t);
        REQUIRE(found != triangles.end());
        CHECK(found->second == before.tags[t]);
    }
}

} // namespace

TEST_CASE("transforms carry a displacement through the editor transform path and save") {
    const auto source = fixture();
    const DVec3 pivot{40, -1500, 32};
    const std::vector<std::pair<const char*, Transform>> cases{
        {"rotate 90 about Z", Transform::rotate({0, 0, 1}, 90, pivot)},
        // Far enough that the stale startposition is nearest a different corner.
        {"translate", Transform::translate({-300, 500, 40})},
        {"mirror X", Transform::mirror({1, 0, 0}, pivot)},
        {"mirror oblique", Transform::mirror({1, 2, 0.5}, pivot)},
        {"non-uniform scale", Transform::scale({2, 0.5, 1.5}, DMat3{Transform::rotate({0, 0, 1}, 30).linear()}, pivot)},
    };
    for (const auto& [name, transform] : cases) {
        const std::string label = name;
        CAPTURE(label);
        Document doc(parsed(source));
        const auto before = meshOf(doc, kSide);
        const auto start = document::displacement(doc, kSide).value()->startPosition;
        const NodeId solid = doc.get(doc.side(kSide)->solid)->id;
        REQUIRE(commitTransform(doc, std::span{&solid, 1}, transform, {}, "Transform"));

        checkPoint(document::displacement(doc, kSide).value()->startPosition, transform.point(start));
        checkCarried(before, meshOf(doc, kSide), transform);

        // Saved and loaded again, the file holds the moved displacement.
        const auto saved = doc.write();
        REQUIRE(saved);
        Document reloaded(parsed(*saved));
        checkCarried(before, meshOf(reloaded, kSide), transform);
        // The out-of-range alphas are carried unchanged (a mirror reorders them).
        if (!transform.reversesOrientation())
            CHECK(saved->find("\"row0\" \"-96 -72 -48 -24 0 24 48 72 0\"") != std::string::npos);

        // Undo restores the source bytes.
        REQUIRE(doc.undo());
        CHECK(doc.write().value() == source);
    }
}
