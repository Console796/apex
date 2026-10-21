#include "fixtures.hpp"
#include "geometry/solid_geometry.hpp"
#include "render/solid_mesh.hpp"
#include "../../tools/replane/vmf_geometry.hpp"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace geometry_test;

namespace {
vmf::VmfFile load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    auto file = vmf::parse(bytes);
    if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
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
        CHECK_FALSE(actual->usedPlaneFallback);
        const auto expected = trusted ? ConvexPolyhedron::fromPointData(vmf_geometry::sides(solid)) :
                                       ConvexPolyhedron::fromPlanes(vmf_geometry::planes(inputs));
        REQUIRE(expected);
        sameVertices(actual->poly, *expected);
        CHECK(actual->poly.bounds().min.x == doctest::Approx(-64 + (trusted ? kPointDataTrustEps / 2 : 0)));
    }
}

TEST_CASE("solidGeometry falls back to planes when trusted point_data topology is invalid") {
    auto file = load(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf");
    auto& solid = file.world->solids[0];
    auto values = solid.sides[0].pointData->findAll("point");
    REQUIRE(values.size() >= 2);
    const auto first = values[0]->text().value();
    REQUIRE(values[1]->set("1" + first.substr(first.find(' '))));
    const auto inputs = vmf_geometry::sides(solid);
    CHECK_FALSE(ConvexPolyhedron::fromPointData(inputs));
    const auto planes = ConvexPolyhedron::fromPlanes(vmf_geometry::planes(inputs));
    REQUIRE(planes);
    const auto actual = solidGeometry(*solid.node);
    REQUIRE(actual);
    CHECK(actual->usedPlaneFallback);
    sameVertices(actual->poly, *planes);
    CHECK(render::meshSolid(*solid.node, [](std::string_view) { return render::TextureSize{}; }));
}

TEST_CASE("solidGeometry accepts unsigned smoothing group bitmasks and signed wrapping") {
    for (const auto& [spelling, expected] :
         std::array<std::pair<const char*, std::uint32_t>, 4>{{
             {"2147483648", UINT32_C(2147483648)}, {"4294967295", UINT32_MAX},
             {"18446744073709551615", UINT32_MAX}, {"-1", UINT32_MAX}}}) {
        auto file = load(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf");
        REQUIRE(file.world->solids[0].sides[0].smoothingGroups()->set(spelling));
        const auto actual = solidGeometry(*file.world->solids[0].node);
        REQUIRE(actual);
        CHECK(actual->sides[0].smoothingGroups == expected);
    }
}

TEST_CASE("solidGeometry and meshSolid retain every plane-renderable P2CE corpus solid") {
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping solid geometry render corpus");
        return;
    }
    const auto root = std::filesystem::path(install) / "sdk_content/maps";
    REQUIRE(std::filesystem::is_directory(root));
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    std::size_t planeAccepted = 0, rendered = 0;
    bool found12485 = false;
    for (const auto& path : files) {
        CAPTURE(path.string());
        auto file = load(path);
        vmf_geometry::visitFile(file, [&](const vmf::Solid& solid) {
            const auto inputs = vmf_geometry::sides(solid);
            const auto planes = ConvexPolyhedron::fromPlanes(vmf_geometry::planes(inputs));
            const auto mesh = render::meshSolid(
                *solid.node, [](std::string_view) { return render::TextureSize{}; });
            if (planes) {
                ++planeAccepted;
                const std::string error = mesh ? std::string{} : mesh.error().what();
                CHECK_MESSAGE(mesh.has_value(), error);
            }
            if (mesh) ++rendered;
            if (path.lexically_relative(root).generic_string() ==
                    "instances/community/elevators/base/elevator_arrival_clean_base.vmf" &&
                solid.id() && solid.id()->asInt().value() == 12485) {
                found12485 = true;
                CHECK_FALSE(ConvexPolyhedron::fromPointData(inputs));
                REQUIRE(planes);
                const auto geometry = solidGeometry(*solid.node);
                REQUIRE(geometry);
                CHECK(geometry->usedPlaneFallback);
                CHECK(mesh);
            }
        });
    }
    CHECK(found12485);
    CHECK(rendered >= planeAccepted);
    MESSAGE("Solid render corpus: ", rendered, " meshed; ", planeAccepted,
            " accepted by plane reconstruction");
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
