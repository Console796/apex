#include "render/solid_mesh.hpp"
#include "geometry/tolerances.hpp"
#include "../../tools/replane/vmf_geometry.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

TEST_CASE("P0 red: CPU mesher agrees with trusted point_data geometry (D4)") {
    using namespace apex;
    std::ifstream input(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf",
                        std::ios::binary);
    REQUIRE(input.good());
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    auto file = vmf::parse(bytes);
    REQUIRE(file);
    auto& solid = file->world->solids[0];
    // Translate every point_data loop together by half the trust tolerance. This keeps
    // topology and planarity valid while differing observably from the unchanged planes.
    const double offset = geometry::kPointDataTrustEps / 2.0;
    for (auto& side : solid.sides) {
        REQUIRE(side.pointData);
        auto points = side.pointData->points();
        REQUIRE(points);
        auto values = side.pointData->findAll("point");
        REQUIRE(values.size() == points->size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            auto point = (*points)[i];
            point[0] += offset;
            REQUIRE(values[i]->set(point));
            auto xyz = values[i]->text();
            REQUIRE(xyz);
            REQUIRE(values[i]->set(std::to_string(i) + " " + *xyz));
        }
    }
    const auto sides = vmf_geometry::sides(solid);
    for (const auto& side : sides) {
        for (const auto point : side.points) {
            REQUIRE(std::abs(side.plane.signedDistance(point)) <= geometry::kPointDataTrustEps);
        }
    }
    auto trusted = geometry::ConvexPolyhedron::fromPointData(sides);
    REQUIRE(trusted);
    REQUIRE(trusted->isValid());
    auto mesh = render::meshSolid(*solid.node, [](std::string_view) { return render::TextureSize{}; });
    REQUIRE(mesh);
    std::vector<DVec3> rendered;
    for (const auto& batch : mesh->batches) {
        for (const auto& vertex : batch.vertices) {
            rendered.push_back(mesh->origin + DVec3{vertex.position[0], vertex.position[1], vertex.position[2]});
        }
    }
    REQUIRE_FALSE(rendered.empty());
    REQUIRE_FALSE(trusted->vertices.empty());
    const auto furthestNearest = [](const auto& from, const auto& to) {
        double worst = 0.0;
        for (const auto point : from) {
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto other : to) nearest = std::min(nearest, glm::length(point - other));
            worst = std::max(worst, nearest);
        }
        return worst;
    };
    const double disagreement = std::max(furthestNearest(rendered, trusted->vertices),
                                         furthestNearest(trusted->vertices, rendered));
    // Local float mesh positions at this fixture's scale have < 1e-5 rounding error.
    // This is well below the intentional 5e-4 point_data displacement.
    CAPTURE(disagreement);
    CAPTURE(offset);
    CHECK_MESSAGE(disagreement < 1e-5,
                  "D4 bug: CPU mesher ignores trusted point_data and reconstructs different vertices from planes");
}
