#include "fixtures.hpp"
#include "editor/work_plane.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace editor_test;

namespace {

void checkFrame(const SurfacePlane& plane) {
    for (const auto vector : {plane.origin, plane.normal, plane.uAxis, plane.vAxis}) {
        CHECK(std::isfinite(vector.x));
        CHECK(std::isfinite(vector.y));
        CHECK(std::isfinite(vector.z));
    }
    CHECK(glm::length(plane.normal) == doctest::Approx(1.0).epsilon(1e-9));
    CHECK(glm::length(plane.uAxis) == doctest::Approx(1.0).epsilon(1e-9));
    CHECK(glm::length(plane.vAxis) == doctest::Approx(1.0).epsilon(1e-9));
    CHECK(std::abs(glm::dot(plane.normal, plane.uAxis)) < 1e-9);
    CHECK(std::abs(glm::dot(plane.normal, plane.vAxis)) < 1e-9);
    CHECK(std::abs(glm::dot(plane.uAxis, plane.vAxis)) < 1e-9);
    CHECK(glm::length(glm::cross(plane.uAxis, plane.vAxis) - plane.normal) < 1e-9);
    if (plane.bounds) {
        CHECK(std::isfinite(plane.bounds->min.x));
        CHECK(std::isfinite(plane.bounds->min.y));
        CHECK(std::isfinite(plane.bounds->max.x));
        CHECK(std::isfinite(plane.bounds->max.y));
        CHECK(plane.bounds->min.x < plane.bounds->max.x);
        CHECK(plane.bounds->min.y < plane.bounds->max.y);
    }
}

void checkBounds(const SurfacePlane& plane, const geometry::ConvexPolyhedron& poly,
                 const geometry::Face& face) {
    REQUIRE(plane.bounds);
    DVec2 min{std::numeric_limits<double>::infinity()}, max{-std::numeric_limits<double>::infinity()};
    for (const auto index : face.vertices) {
        const auto relative = poly.vertices[index] - plane.origin;
        const DVec2 point{glm::dot(relative, plane.uAxis), glm::dot(relative, plane.vAxis)};
        min = glm::min(min, point);
        max = glm::max(max, point);
        CHECK(std::abs(glm::dot(relative, plane.normal)) < 1e-6);
    }
    CHECK(glm::length(plane.bounds->min - min) < 1e-9);
    CHECK(glm::length(plane.bounds->max - max) < 1e-9);
}

} // namespace

TEST_CASE("work_plane GridLadder steps through every power of two and clamps") {
    double up = GridLadder::kMin, down = GridLadder::kMax;
    // 14 entries, hence 13 transitions between the inclusive endpoints.
    for (int step = 0; step < 13; ++step) {
        CHECK(GridLadder::isValid(up));
        CHECK(GridLadder::isValid(down));
        CHECK(GridLadder::clamp(up) == up);
        CHECK(GridLadder::clamp(down) == down);
        CHECK(GridLadder::larger(up) == up * 2.0);
        CHECK(GridLadder::smaller(down) == down * 0.5);
        up = GridLadder::larger(up);
        down = GridLadder::smaller(down);
    }
    CHECK(up == GridLadder::kMax);
    CHECK(down == GridLadder::kMin);
    CHECK(GridLadder::larger(up) == up);
    CHECK(GridLadder::smaller(down) == down);
    for (const double value : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        CHECK_FALSE(GridLadder::isValid(value));
        CHECK(GridLadder::clamp(value) == GridLadder::kDefault);
    }
    for (const double value : {0.124, 1024.001, 3.0, std::nextafter(64.0, 65.0)})
        CHECK_FALSE(GridLadder::isValid(value));
    CHECK(GridLadder::clamp(std::numeric_limits<double>::denorm_min()) == 0.125);
    CHECK(GridLadder::clamp(std::numeric_limits<double>::max()) == 1024.0);
    CHECK(GridLadder::clamp(0.1874) == 0.125);
    CHECK(GridLadder::clamp(0.1875) == 0.25);
    CHECK(GridLadder::clamp(47.9) == 32.0);
    CHECK(GridLadder::clamp(48.0) == 64.0);
    CHECK(GridLadder::clamp(65.0) == 64.0);
}

TEST_CASE("work_plane box faces use ordered world axes and world-anchored rectangles") {
    Document doc(parsed());
    const DVec3 min{-12.0, -20.0, -8.0}, max{28.0, 60.0, 16.0};
    const auto solid = addBox(doc, min, max);
    const auto geometry = geometry::solidGeometry(*doc.get(solid)->block);
    REQUIRE(geometry);
    const auto& poly = geometry->poly;
    for (const auto& face : poly.faces) {
        CAPTURE(face.sideId);
        const auto plane = planeFromFace(doc, id(doc, solid), face.sideId);
        REQUIRE(plane);
        checkFrame(*plane);
        checkBounds(*plane, poly, face);
        const bool x = std::abs(face.plane.normal.x) == 1.0;
        const bool y = std::abs(face.plane.normal.y) == 1.0;
        const DVec3 u = x ? DVec3{0, 1, 0} : DVec3{1, 0, 0};
        DVec3 v = x || y ? DVec3{0, 0, 1} : DVec3{0, 1, 0};
        if (glm::dot(glm::cross(u, v), face.plane.normal) < 0.0) v = -v;
        CHECK(plane->uAxis == u);
        CHECK(plane->vAxis == v);
        CHECK(plane->normal == face.plane.normal);
        // Anchored at the world origin's projection, so plane coordinates are world u/v coordinates.
        checkPoint(plane->origin, face.plane.normal * face.plane.dist);
        const DVec2 lo{glm::dot(u, glm::dot(u, DVec3{1.0}) > 0.0 ? min : max),
                       glm::dot(v, glm::dot(v, DVec3{1.0}) > 0.0 ? min : max)};
        const DVec2 hi{glm::dot(u, glm::dot(u, DVec3{1.0}) > 0.0 ? max : min),
                       glm::dot(v, glm::dot(v, DVec3{1.0}) > 0.0 ? max : min)};
        CHECK(glm::length(plane->bounds->min - lo) < 1e-9);
        CHECK(glm::length(plane->bounds->max - hi) < 1e-9);
    }
    CHECK_FALSE(planeFromFace(doc, id(doc, solid), SideId{999}));
    CHECK_FALSE(planeFromFace(doc, NodeId{999}, SideId{11}));
    const auto entity = addBlock(doc, "entity { }");
    CHECK_FALSE(planeFromFace(doc, id(doc, entity), SideId{11}));
    const auto malformed = addBlock(doc, "solid { side { \"id\" \"100\" } }");
    CHECK_FALSE(planeFromFace(doc, id(doc, malformed), SideId{100}));
}

TEST_CASE("work_plane sloped faces retain orthonormal frames and polygon bounds") {
    for (const auto& sides : {geometry_test::wedgeSides(), geometry_test::offGridSides()}) {
        const auto poly = geometry_test::build(sides);
        Document doc(parsed());
        auto tx = doc.begin("slope");
        const auto solid = tx.create(solidBlock(poly));
        tx.commit();
        const auto saved = geometry::solidGeometry(*doc.get(solid)->block);
        REQUIRE(saved);
        for (const auto& face : saved->poly.faces) {
            const auto plane = planeFromFace(doc, id(doc, solid), static_cast<SideId>(face.sideId));
            REQUIRE(plane);
            checkFrame(*plane);
            checkBounds(*plane, saved->poly, face);
            CHECK(glm::dot(plane->normal, face.plane.normal) > 1.0 - 1e-9);
        }
    }
}

TEST_CASE("work_plane tiny and distant faces remain finite and orthonormal") {
    for (const double coordinate : {0.0, -65536.0, 65536.0}) {
        for (const double size : {1e-3, 128.0}) {
            Document doc(parsed());
            const auto solid = addBox(doc, DVec3{coordinate}, DVec3{coordinate + size});
            const auto geometry = geometry::solidGeometry(*doc.get(solid)->block);
            REQUIRE(geometry);
            for (const auto& face : geometry->poly.faces) {
                const auto plane = planeFromFace(doc, id(doc, solid), static_cast<SideId>(face.sideId));
                REQUIRE(plane);
                checkFrame(*plane);
                CHECK(plane->bounds->max.x - plane->bounds->min.x == doctest::Approx(size).epsilon(1e-7));
                CHECK(plane->bounds->max.y - plane->bounds->min.y == doctest::Approx(size).epsilon(1e-7));
            }
        }
    }
    geometry::ConvexPolyhedron poly;
    geometry::Face face;
    CHECK_FALSE(planeFromFace(poly, face));
    poly.vertices = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    face.vertices = {0, 1, 2};
    CHECK_FALSE(planeFromFace(poly, face));
    face.vertices = {0, 1, 99};
    CHECK_FALSE(planeFromFace(poly, face));
    poly.vertices[2] = {0, std::numeric_limits<double>::quiet_NaN(), 0};
    face.vertices = {0, 1, 2};
    CHECK_FALSE(planeFromFace(poly, face));
}

TEST_CASE("work_plane state resolves pinned before hovered and leaves fallback to viewport") {
    WorkPlaneState state;
    CHECK_FALSE(state.resolve());
    state.pinHovered();
    CHECK_FALSE(state.pinned());
    const SurfacePlane first{{1, 2, 3}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, Rect2D{{-2, -3}, {2, 3}}};
    auto other = first;
    other.origin = {4, 5, 6};
    state.setHovered(first);
    REQUIRE(state.resolve());
    CHECK(state.resolve()->source == WorkPlaneSource::Hovered);
    CHECK(state.resolve()->plane.origin == first.origin);
    state.pinHovered();
    CHECK(state.pinned());
    state.setHovered(other);
    CHECK(state.resolve()->source == WorkPlaneSource::Pinned);
    CHECK(state.resolve()->plane.origin == first.origin);
    CHECK(state.resolve()->plane.bounds->min == first.bounds->min);
    state.unpin();
    CHECK_FALSE(state.pinned());
    CHECK(state.resolve()->source == WorkPlaneSource::Hovered);
    CHECK(state.resolve()->plane.origin == other.origin);
    state.pinHovered();
    state.setHovered(std::nullopt);
    state.pinHovered(); // No hovered plane must leave the existing pin intact.
    CHECK(state.resolve()->plane.origin == other.origin);
    state.unpin();
    CHECK_FALSE(state.resolve());
    for (const auto axis : {Axis::X, Axis::Y, Axis::Z}) {
        state.pinAxisPlane(axis, {7, 8, 9});
        const auto resolved = state.resolve();
        REQUIRE(resolved);
        CHECK(state.pinned());
        CHECK(resolved->source == WorkPlaneSource::Pinned);
        CHECK(resolved->plane.origin == DVec3{7, 8, 9});
        CHECK_FALSE(resolved->plane.bounds);
        const DVec3 normal = axis == Axis::X ? DVec3{1, 0, 0} : axis == Axis::Y ? DVec3{0, 1, 0} : DVec3{0, 0, 1};
        CHECK(resolved->plane.normal == normal);
        CHECK(resolved->plane.uAxis == (axis == Axis::X ? DVec3{0, 1, 0} : DVec3{1, 0, 0}));
        CHECK(resolved->plane.vAxis == (axis == Axis::Z ? DVec3{0, 1, 0} : axis == Axis::X ? DVec3{0, 0, 1} : DVec3{0, 0, -1}));
        checkFrame(resolved->plane);
    }
}

TEST_CASE("work_plane P2CE corpus sample visible solid faces have finite planes") {
    namespace fs = std::filesystem;
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping work_plane corpus sample");
        return;
    }
    const auto root = fs::path(install) / "sdk_content/maps";
    REQUIRE(fs::is_directory(root));
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") paths.push_back(entry.path());
    }
    std::ranges::sort(paths);
    REQUIRE_FALSE(paths.empty());
    // Same document walk/parser as selection_corpus_test; bound this slice to three maps.
    paths.resize(std::min(paths.size(), std::size_t{3}));
    std::size_t faces = 0, rejected = 0, discarded = 0;
    for (const auto& path : paths) {
        CAPTURE(path.string());
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
        auto file = vmf::parse(bytes);
        if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
        REQUIRE(file);
        Document doc(*file);
        for (const auto h : nodes(doc)) {
            if (!std::holds_alternative<Handle<Solid>>(h) || !isVisible(doc, id(doc, h))) continue;
            const auto geometry = geometry::solidGeometry(*doc.get(h)->block);
            // Invalid solids have no geometry; redundant/coplanar input sides may be
            // discarded by reconstruction. Both yield documented nullopt at the DOM API.
            if (!geometry) { ++rejected; continue; }
            for (const auto& side : geometry->sides) {
                const auto face = std::ranges::find(geometry->poly.faces, side.sideId, &geometry::Face::sideId);
                if (face == geometry->poly.faces.end()) { ++discarded; continue; }
                const auto plane = planeFromFace(geometry->poly, *face);
                REQUIRE(plane);
                checkFrame(*plane);
                checkBounds(*plane, geometry->poly, *face);
                ++faces;
            }
        }
    }
    CHECK(faces > 0);
    MESSAGE("work_plane corpus sample: " << faces << " faces, " << rejected
            << " invalid solids, " << discarded << " redundant sides");
}
