#include "document/document.hpp"
#include "geometry/convex_polyhedron.hpp"
#include "geometry/tolerances.hpp"
#include "../../tools/replane/vmf_geometry.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>

namespace {
using namespace apex;
using namespace apex::document;

vmf::VmfFile load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_MESSAGE(input.good(), path.string());
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    auto file = vmf::parse(bytes);
    if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
    REQUIRE_MESSAGE(file, path.string());
    return std::move(*file);
}

vmf::VmfFile brushFixture() {
    return load(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/p0_brush.vmf");
}

vmf::Vec3 rotate(vmf::Vec3 point) {
    const double angle = std::numbers::pi / 6.0;
    return {12345.0 + std::cos(angle) * point[0] - std::sin(angle) * point[1],
            std::sin(angle) * point[0] + std::cos(angle) * point[1], point[2]};
}
} // namespace

TEST_CASE("P0 red: creating 5000 solids on the largest corpus map scales linearly (D17)") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    REQUIRE_MESSAGE((install && *install), "P0 D17 requires APEX_P2CE_DIR pointing to the P2CE corpus");
    const auto root = std::filesystem::path(install) / "sdk_content/maps";
    REQUIRE(std::filesystem::is_directory(root));
    std::filesystem::path largest;
    std::uintmax_t largestBytes = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".vmf") continue;
        const auto bytes = entry.file_size();
        if (bytes > largestBytes || (bytes == largestBytes && entry.path() < largest)) {
            largest = entry.path();
            largestBytes = bytes;
        }
    }
    REQUIRE_FALSE(largest.empty());
    CAPTURE(largest.string());
    auto corpus = load(largest);
    auto fixture = brushFixture();
    const kv1::Node solid = *fixture.world->solids[0].node;
    const auto elapsed = [&](int count) {
        Document doc(corpus); // Parsing, import and destruction are outside the timer.
        std::optional<NodeHandle> world;
        for (const auto id : doc.roots()) {
            const auto h = *doc.handle(id);
            if (doc.get(h)->block->key() == "world") world = h;
        }
        REQUIRE(world);
        const auto start = std::chrono::steady_clock::now();
        auto tx = doc.begin("bulk solid creation");
        for (int i = 0; i < count; ++i) tx.create(solid, world);
        const auto changes = tx.commit();
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        REQUIRE(changes.created.size() == static_cast<std::size_t>(count));
        return seconds;
    };
    const double time1000 = elapsed(1000);
    const double time5000 = elapsed(5000);
    REQUIRE(time1000 > 0.0);
    const double ratio = time5000 / time1000;
    CAPTURE(time1000);
    CAPTURE(time5000);
    CAPTURE(ratio);
    CHECK_MESSAGE(ratio < 8.0,
                  "D17 bug: bulk solid creation is quadratic; time(5000)/time(1000) must be < 8");
}

TEST_CASE("P0 red: rotated brush point_data stays within its saved plane trust tolerance (D18)") {
    auto file = brushFixture();
    auto& solid = file.world->solids[0];
    const auto original = vmf_geometry::sides(solid);
    REQUIRE(geometry::ConvexPolyhedron::fromPointData(original));
    std::vector<std::pair<SideId, std::string>> planes;
    for (auto& side : solid.sides) {
        auto plane = side.plane()->asPlane();
        REQUIRE(plane);
        for (auto& point : plane->points) point = rotate(point);
        const auto exactPlane = geometry::planePointsFromSource(
            vmf_geometry::vector(plane->points[0]), vmf_geometry::vector(plane->points[1]),
            vmf_geometry::vector(plane->points[2]));
        REQUIRE(exactPlane);
        REQUIRE(side.pointData);
        auto points = side.pointData->points();
        REQUIRE(points);
        for (const auto point : *points) {
            REQUIRE(std::abs(exactPlane->signedDistance(vmf_geometry::vector(rotate(point)))) <=
                    geometry::kPlanarEps);
        }
        REQUIRE(side.plane()->set(*plane)); // Exercise the production number formatter.
        auto text = side.plane()->text();
        REQUIRE(text);
        planes.emplace_back(*side.id()->asInt(), *text);

        auto values = side.pointData->findAll("point");
        REQUIRE(values.size() == points->size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            // There is no transaction API for nested point_data yet. Use the existing VMF
            // setter to format xyz, then restore the point index required by the codec.
            REQUIRE(values[i]->set(rotate((*points)[i])));
            auto xyz = values[i]->text();
            REQUIRE(xyz);
            REQUIRE(values[i]->set(std::to_string(i) + " " + *xyz));
        }
    }
    // Import transformed point_data with the original planes, then exercise the current
    // transaction side-write path. No new geometry-writing API is needed for this repro.
    auto originalFile = brushFixture();
    for (std::size_t i = 0; i < solid.sides.size(); ++i) {
        REQUIRE(solid.sides[i].plane()->set(*originalFile.world->solids[0].sides[i].plane()->text()));
    }
    Document doc(file);
    auto tx = doc.begin("rotate brush 30 degrees at x=12345");
    for (const auto& [id, plane] : planes) tx.setSideKeyValue(id, "plane", plane);
    tx.commit();
    auto bytes = doc.write();
    REQUIRE(bytes);
    auto reloaded = vmf::parse(*bytes);
    REQUIRE(reloaded);
    REQUIRE(reloaded->world);
    REQUIRE(reloaded->world->solids.size() == 1);
    double maxDistance = 0.0;
    for (const auto& side : vmf_geometry::sides(reloaded->world->solids[0])) {
        REQUIRE_FALSE(side.points.empty());
        for (const auto point : side.points) {
            maxDistance = std::max(maxDistance, std::abs(side.plane.signedDistance(point)));
        }
    }
    CAPTURE(maxDistance);
    CAPTURE(geometry::kPointDataTrustEps);
    CHECK_MESSAGE(maxDistance <= geometry::kPointDataTrustEps,
                  "D18 bug: six-significant-digit geometry writes push rotated point_data off its own plane");
}
