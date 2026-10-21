#include "fixtures.hpp"
#include "geometry/solid_geometry.hpp"
#include "../../tools/replane/vmf_geometry.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace geometry_test;

namespace {
vmf::VmfFile load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    auto file = vmf::parse(std::string{std::istreambuf_iterator<char>(input), {}});
    REQUIRE(file);
    return std::move(*file);
}
} // namespace

TEST_CASE("solidGeometry matches point_data construction and preserves side inputs") {
    std::size_t solids = 0, invalid = 0;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(APEX_FIXTURE_DIR) / "vmf")) {
        if (entry.path().extension() != ".vmf") continue;
        CAPTURE(entry.path().string());
        auto file = load(entry.path());
        vmf_geometry::visitFile(file, [&](const vmf::Solid& solid) {
            ++solids;
            const auto inputs = vmf_geometry::sides(solid);
            const auto expected = ConvexPolyhedron::fromPointData(inputs);
            const auto actual = solidGeometry(*solid.node);
            REQUIRE(actual.has_value() == expected.has_value());
            if (!actual) {
                ++invalid;
                CHECK(entry.path().filename() == "everything_lf.vmf");
                return;
            }
            sameVertices(actual->poly, *expected);
            REQUIRE(actual->sides.size() == inputs.size());
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                sameAttributes(actual->sides[i], inputs[i]);
                CHECK(actual->sides[i].plane.normal == inputs[i].plane.normal);
                CHECK(actual->sides[i].plane.dist == inputs[i].plane.dist);
                CHECK(actual->sides[i].points == inputs[i].points);
            }
        });
    }
    CHECK(solids == 7);
    CHECK(invalid == 2); // Schema fixtures deliberately have incomplete solids.
}

TEST_CASE("solidGeometry trusts nearby point_data and falls back beyond the trust tolerance") {
    for (const bool trusted : {true, false}) {
        auto file = load(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf");
        auto& solid = file.world->solids[0];
        const auto inputs = vmf_geometry::sides(solid);
        for (std::size_t i = 0; i < solid.sides.size(); ++i) {
            auto& side = solid.sides[i];
            auto points = side.pointData->points().value();
            auto values = side.pointData->findAll("point");
            const DVec3 offset = trusted ? DVec3{kPointDataTrustEps / 2, 0, 0} :
                                          inputs[i].plane.normal * (2 * kPointDataTrustEps);
            for (std::size_t j = 0; j < values.size(); ++j) {
                const DVec3 point = vmf_geometry::vector(points[j]) + offset;
                REQUIRE(values[j]->setGeometry(vmf::Vec3{point.x, point.y, point.z}));
                REQUIRE(values[j]->set(std::to_string(j) + " " + values[j]->text().value()));
            }
        }
        const auto actual = solidGeometry(*solid.node);
        REQUIRE(actual);
        const auto expected = trusted ? ConvexPolyhedron::fromPointData(vmf_geometry::sides(solid)) :
                                       ConvexPolyhedron::fromPlanes(vmf_geometry::planes(inputs));
        REQUIRE(expected);
        sameVertices(actual->poly, *expected);
        CHECK(actual->poly.bounds().min.x == doctest::Approx(-64 + (trusted ? kPointDataTrustEps / 2 : 0)));
    }
}

TEST_CASE("solidGeometry reports malformed geometry and texture attributes") {
    for (const auto* key : {"plane", "id", "uaxis", "vaxis", "lightmapscale", "smoothing_groups"}) {
        auto file = load(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf");
        auto& solid = file.world->solids[0];
        CAPTURE(key);
        REQUIRE(solid.sides[0].find(key)->set("invalid"));
        CHECK_FALSE(solidGeometry(*solid.node));
    }
}
