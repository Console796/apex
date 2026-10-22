#include "fixtures.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <tuple>

using namespace editor_test;

TEST_CASE("Pick nested coplanar brushes edge corner and inside rays use double precision") {
    for (const double offset : {-65536.0, 0.0, 65536.0}) {
        CAPTURE(offset);
        Document doc(parsed());
        const DVec3 translation{offset, 0, 0};
        const auto outer = addBox(doc, translation + DVec3{-8}, translation + DVec3{8});
        const auto inner = addBox(doc, translation + DVec3{-4}, translation + DVec3{4});
        const auto same = addBox(doc, translation + DVec3{-8}, translation + DVec3{8});
        PickIndex index(doc);
        for (const auto yz : {DVec2{0, 0}, DVec2{8, 0}, DVec2{8, 8}}) {
            const Ray ray{translation + DVec3{-20, yz.x, yz.y}, {2, 0, 0}};
            const auto hits = index.pick(ray);
            REQUIRE(hits.size() == (yz.x == 0 ? 3 : 2));
            CHECK(hits[0].node == id(doc, outer));
            CHECK(hits[0].t == doctest::Approx(6));
            CHECK(hits[0].side);
            checkPoint(hits[0].point, translation + DVec3{-8, yz.x, yz.y});
            checkPoint(hits[0].normal, {-1, 0, 0});
            CHECK(hits[1].node == id(doc, same));
            REQUIRE(index.first(ray));
            CHECK(index.first(ray)->node == hits[0].node);
            CHECK(index.pick(ray, 5.999).empty());
            CHECK(index.pick(ray, 6).size() == 2);
        }
        const auto inside = index.pick({translation, {1, 0, 0}});
        REQUIRE(inside.size() == 3);
        CHECK(inside[0].node == id(doc, inner));
        CHECK(inside[0].t == doctest::Approx(4));
        checkPoint(inside[0].normal, {1, 0, 0});
        const auto corner = index.first({translation + DVec3{-20}, {1, 1, 1}});
        REQUIRE(corner);
        const auto poly = geometry::solidGeometry(*doc.get(outer)->block);
        REQUIRE(poly);
        SideId minSide = std::numeric_limits<SideId>::max();
        for (const auto& face : poly->poly.faces) {
            if (face.plane.signedDistance(translation + DVec3{-8}) == 0) {
                minSide = std::min(minSide, static_cast<SideId>(face.sideId));
            }
        }
        CHECK(corner->side == minSide);
        CHECK(index.pick({translation, {0, 0, 0}}).empty());
        CHECK(index.pick({translation, {1, 0, 0}}, -1).empty());
    }
}

TEST_CASE("Pick entity provider boxes defaults world and brush entities") {
    Document doc(parsed());
    addBlock(doc, "world { classname worldspawn }");
    const auto point = addBlock(doc, "entity { classname info_target origin \"20 0 0\" }");
    const auto custom = addBlock(doc, "entity { classname custom origin \"40 0 0\" }");
    const auto brush = addBlock(doc, "entity { classname func_detail }");
    const auto wrapper = addBlock(doc, "hidden {}", brush);
    addBox(doc, DVec3{-8}, DVec3{8}, wrapper);
    PickIndex index(doc, [](std::string_view classname) -> std::optional<LocalBox> {
        if (classname == "custom") return LocalBox{{-2, -3, -4}, {6, 7, 8}};
        return std::nullopt;
    });
    CHECK(index.size() == 2);
    const auto hits = index.pick({{0, 0, 0}, {1, 0, 0}});
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].node == id(doc, point));
    CHECK_FALSE(hits[0].side);
    CHECK(hits[0].t == doctest::Approx(16));
    CHECK(hits[1].node == id(doc, custom));
    CHECK(hits[1].t == doctest::Approx(38));
    checkPoint(hits[1].normal, {-1, 0, 0});
    REQUIRE(index.first({{20, 0, 0}, {1, 0, 0}}));
    CHECK(index.first({{20, 0, 0}, {1, 0, 0}})->t == doctest::Approx(4));
    auto tx = doc.begin("move point");
    tx.setKeyValue(point, "origin", "80 0 0");
    index.apply(tx.commit());
    CHECK(index.first({{0, 0, 0}, {1, 0, 0}})->node == id(doc, custom));
}

TEST_CASE("Pick updates geometry deletion undo redo hierarchy and observer lifetime") {
    Document doc(parsed());
    const auto moving = addBox(doc, DVec3{-8}, DVec3{8});
    const auto movingId = id(doc, moving);
    for (int i = 0; i < 12; ++i) addBox(doc, {100.0 + 20 * i, -8, -8}, {110.0 + 20 * i, 8, 8});
    const auto hidden = addBlock(doc, "hidden {}");
    PickIndex index(doc);
    index.attach(doc);
    index.attach(doc);
    auto geometry = geometry::solidGeometry(*doc.get(moving)->block);
    REQUIRE(geometry);
    translate(*geometry, {0, 100, 0});
    auto tx = doc.begin("move solid");
    tx.setSolidGeometry(std::get<Handle<Solid>>(moving), *geometry);
    tx.commit();
    CHECK(index.first({{-20, 0, 0}, {1, 0, 0}})->node != movingId);
    CHECK(index.first({{-20, 100, 0}, {1, 0, 0}})->node == movingId);
    REQUIRE(doc.undo());
    CHECK(index.first({{-20, 0, 0}, {1, 0, 0}})->node == movingId);
    REQUIRE(doc.redo());
    CHECK(index.first({{-20, 100, 0}, {1, 0, 0}})->node == movingId);
    auto hide = doc.begin("reparent hidden");
    hide.reparent(moving, hidden);
    hide.commit();
    CHECK_FALSE(index.first({{-20, 100, 0}, {1, 0, 0}}));
    REQUIRE(doc.undo());
    auto erase = doc.begin("delete");
    erase.destroy(moving);
    erase.commit();
    CHECK_FALSE(index.first({{-20, 100, 0}, {1, 0, 0}}));
    REQUIRE(doc.undo());
    CHECK(*doc.handle(movingId) != moving);
    CHECK(index.first({{-20, 100, 0}, {1, 0, 0}})->node == movingId);
    REQUIRE(doc.redo());
    CHECK_FALSE(index.first({{-20, 100, 0}, {1, 0, 0}}));
    index.detach();
    REQUIRE(doc.undo());
    CHECK_FALSE(index.first({{-20, 100, 0}, {1, 0, 0}}));
    index.attach(doc);
    CHECK(index.first({{-20, 100, 0}, {1, 0, 0}}));
    { PickIndex temporary(doc); temporary.attach(doc); }
    auto edit = doc.begin("after destructor");
    edit.setKeyValue(*doc.handle(movingId), "note", "safe");
    edit.commit();
    Document other(parsed());
    CHECK_THROWS_AS(index.attach(other), std::invalid_argument);
}

TEST_CASE("Pick invalid geometry skipped count and vanished sides stay current") {
    Document doc(parsed());
    PickIndex index(doc);
    index.attach(doc);
    const auto invalid = addBlock(doc, "solid { side { plane broken } }");
    CHECK(index.skippedSolids() == 1);
    CHECK(index.size() == 0);
    const auto good = addBox(doc, DVec3{-8}, DVec3{8});
    auto erase = doc.begin("delete invalid");
    erase.destroy(invalid);
    erase.commit();
    CHECK(index.skippedSolids() == 0);
    auto tx = doc.begin("break plane");
    const auto poly = geometry::solidGeometry(*doc.get(good)->block);
    REQUIRE(poly);
    tx.setSideKeyValue(static_cast<SideId>(poly->poly.faces[0].sideId), "plane", "broken");
    index.apply(tx.commit());
    CHECK(index.size() == 0);
    CHECK(index.skippedSolids() == 1);
    REQUIRE(doc.undo());
    CHECK(index.size() == 1);
    auto deletion = doc.begin("vanish sides");
    deletion.destroy(good);
    const auto changes = deletion.commit();
    CHECK_FALSE(changes.vanishedSides.empty());
    index.apply(changes);
    CHECK(index.size() == 0);
}

TEST_CASE("Pick incremental inserts removals and entity hierarchy changes avoid unrelated rebuilds") {
    Document doc(parsed());
    for (int i = 0; i < 12; ++i) {
        addBlock(doc, "entity { classname other origin \"100 100 100\" }");
    }
    const auto entity = addBlock(doc, "entity { classname target }");
    std::size_t providerCalls = 0;
    PickIndex index(doc, [&](std::string_view) -> std::optional<LocalBox> {
        ++providerCalls;
        return std::nullopt;
    });
    index.attach(doc);
    providerCalls = 0;
    const auto solid = addBox(doc, DVec3{-8}, DVec3{8}, entity);
    const auto solidId = id(doc, solid);
    CHECK(providerCalls == 0); // Only the changed entity subtree was reconsidered.
    CHECK(index.size() == 13); // Its entity box was replaced by the solid.
    const Ray ray{{-20, 0, 0}, {1, 0, 0}};
    REQUIRE(index.first(ray));
    CHECK(index.first(ray)->node == solidId);
    auto tx = doc.begin("remove brush");
    tx.destroy(solid);
    tx.commit();
    CHECK(providerCalls == 1);
    REQUIRE(index.first(ray));
    CHECK(index.first(ray)->node == id(doc, entity));
    CHECK_FALSE(index.first(ray)->side);
    REQUIRE(doc.undo());
    CHECK(index.first(ray)->node == solidId);
    CHECK(index.size() == 13);
}

TEST_CASE("Pick exact sloping faces reject box false positives and clip oblique frusta") {
    Document doc(parsed());
    const auto wedge = geometry_test::build(geometry_test::wedgeSides());
    auto tx = doc.begin("wedge");
    const auto handle = tx.create(solidBlock(wedge));
    tx.commit();
    PickIndex index(doc);
    CHECK_FALSE(index.first({{60, 60, 40}, {0, 0, -1}})); // Inside AABB, outside x+y<=64.
    const auto hit = index.first({{60, 60, 16}, {-1, -1, 0}});
    REQUIRE(hit);
    CHECK(hit->node == id(doc, handle));
    CHECK(hit->t == doctest::Approx(28));
    checkPoint(hit->point, {32, 32, 16});
    checkPoint(hit->normal, glm::normalize(DVec3{1, 1, 0}));
    auto frustum = planes({50, 50, 0}, {60, 60, 32});
    CHECK(index.frustum(frustum, Mode::Touching).empty());
    frustum = planes({31, 31, 8}, {35, 35, 24});
    CHECK(index.frustum(frustum, Mode::Enclosed).empty());
    CHECK(index.frustum(frustum, Mode::Touching) == std::vector<NodeId>{id(doc, handle)});
    const auto near = index.nearest({40, 40, 16}, 12);
    REQUIRE(near.size() == 1);
    checkPoint(near[0].point, {32, 32, 16});
    CHECK(near[0].distance == doctest::Approx(std::sqrt(128.0)));
    const std::vector<Plane> oblique{{glm::normalize(DVec3{1, 1, 0}), 60.0}};
    CHECK(index.frustum(oblique, Mode::Enclosed) == std::vector<NodeId>{id(doc, handle)});
}

TEST_CASE("Pick frustum enclosed touching clipping and nearest surface") {
    Document doc(parsed());
    const auto a = addBox(doc, {-8, -8, -8}, {8, 8, 8});
    const auto b = addBox(doc, {10, -8, -8}, {20, 8, 8});
    const auto point = addBlock(doc, "entity { classname info_target origin \"40 0 0\" }");
    PickIndex index(doc);
    const auto volume = planes({-9, -9, -9}, {12, 9, 9});
    CHECK(index.frustum(volume, Mode::Enclosed) == std::vector<NodeId>{id(doc, a)});
    CHECK(index.frustum(volume, Mode::Touching) == std::vector<NodeId>{id(doc, a), id(doc, b)});
    CHECK(index.frustum(planes(DVec3{-1}, DVec3{1}), Mode::Touching).empty());
    CHECK(index.frustum(planes({39, -1, -1}, {41, 1, 1}), Mode::Touching) ==
          std::vector<NodeId>{id(doc, point)});
    CHECK(index.frustum(planes({39, -1, -1}, {41, 1, 1}), Mode::Enclosed).empty());
    CHECK(index.frustum(planes({8, -1, -1}, {9, 1, 1}), Mode::Touching) == std::vector<NodeId>{id(doc, a)});
    CHECK(index.frustum({}, Mode::Enclosed).size() == 3);
    const auto near = index.nearest({9, 0, 0}, 1);
    REQUIRE(near.size() == 2);
    CHECK(near[0].node == id(doc, a));
    CHECK(near[1].node == id(doc, b));
    CHECK(near[0].distance == doctest::Approx(1));
    checkPoint(near[0].point, {8, 0, 0});
    checkPoint(near[1].point, {10, 0, 0});
    const auto corner = index.nearest({9, 9, 9}, std::sqrt(3.0) + 1e-9);
    REQUIRE(corner.size() == 2);
    checkPoint(corner[0].point, {8, 8, 8});
    CHECK(index.nearest({0, 0, 0}, 7.9).empty());
    CHECK(index.nearest({0, 0, 0}, 8).size() == 1);
    CHECK(index.nearest({0, 0, 0}, -1).empty());
    const auto entityNear = index.nearest({45, 0, 0}, 1);
    REQUIRE(entityNear.size() == 1);
    CHECK(entityNear[0].node == id(doc, point));
    checkPoint(entityNear[0].point, {44, 0, 0});
}

namespace {

struct BruteSolid {
    NodeId id;
    geometry::ConvexPolyhedron poly;
};

std::optional<Hit> bruteFirst(const std::vector<BruteSolid>& solids, Ray ray) {
    std::optional<Hit> best;
    // Deliberately no spatial broad phase. Fan triangles are an independent polygon test.
    for (const auto& solid : solids) {
        for (const auto& face : solid.poly.faces) {
            const double denominator = glm::dot(face.plane.normal, ray.dir);
            if (denominator == 0.0) continue;
            // Plane-derived t makes shared/coplanar face ties exact; triangle-derived t
            // has a different rounding path and would randomly choose another solid.
            const double t = -face.plane.signedDistance(ray.origin) / denominator;
            if (t < 0.0) continue;
            const auto a = solid.poly.vertices[face.vertices[0]];
            for (std::size_t i = 1; i + 1 < face.vertices.size(); ++i) {
                const auto e1 = solid.poly.vertices[face.vertices[i]] - a;
                const auto e2 = solid.poly.vertices[face.vertices[i + 1]] - a;
                const auto p = glm::cross(ray.dir, e2);
                const double det = glm::dot(e1, p);
                if (det == 0.0) continue;
                const auto s = ray.origin - a;
                const double u = glm::dot(s, p) / det;
                if (u < 0.0 || u > 1.0) continue;
                const auto q = glm::cross(s, e1);
                const double v = glm::dot(ray.dir, q) / det;
                if (v < 0.0 || u + v > 1.0) continue;
                Hit hit{solid.id, static_cast<SideId>(face.sideId), ray.at(t), face.plane.normal, t};
                if (!best || std::tie(hit.t, hit.node, hit.side) < std::tie(best->t, best->node, best->side)) {
                    best = hit;
                }
            }
        }
    }
    return best;
}

} // namespace

TEST_CASE("Pick P2CE corpus 10000 rays per map match brute force and report build refit timing") {
    namespace fs = std::filesystem;
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping Pick corpus comparison and timings");
        return;
    }
    const fs::path root = fs::path(install) / "sdk_content/maps";
    REQUIRE(fs::is_directory(root));
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") paths.push_back(entry.path());
    }
    std::ranges::sort(paths, [](const fs::path& a, const fs::path& b) {
        const auto as = fs::file_size(a), bs = fs::file_size(b);
        return as != bs ? as > bs : a.generic_string() < b.generic_string();
    });
    REQUIRE(paths.size() >= 4);
    // Largest and three other substantial maps, with deterministic ordering and seeds.
    for (std::size_t map = 0; map < 4; ++map) {
        const auto& path = paths[map];
        CAPTURE(path.string());
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
        auto file = vmf::parse(bytes);
        if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
        REQUIRE(file);
        Document doc(*file);
        const auto start = std::chrono::steady_clock::now();
        PickIndex index(doc);
        const auto buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::vector<BruteSolid> solids;
        AABB bounds;
        ViewState onlySolids;
        onlySolids.isolate.emplace();
        std::size_t rejected = 0;
        for (const auto h : nodes(doc)) {
            if (!std::holds_alternative<Handle<Solid>>(h)) continue;
            const auto* node = doc.get(h);
            if (!isVisible(doc, node->id)) continue;
            auto geometry = geometry::solidGeometry(*node->block);
            if (!geometry) { ++rejected; continue; }
            bounds.expand(geometry->poly.bounds());
            onlySolids.isolate->insert(node->id);
            solids.push_back({node->id, std::move(geometry->poly)});
        }
        CHECK(index.skippedSolids() == rejected);
        REQUIRE_FALSE(solids.empty());
        index.setViewState(std::move(onlySolids));
        CHECK(index.size() == solids.size());
        std::mt19937_64 random(0x022B + map);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::uniform_int_distribution<std::size_t> target(0, solids.size() - 1);
        for (int sample = 0; sample < 10000; ++sample) {
            CAPTURE(sample);
            const DVec3 origin = bounds.min + DVec3{unit(random), unit(random), unit(random)} * (bounds.max - bounds.min);
            const auto direction = solids[target(random)].poly.center() - origin;
            REQUIRE(glm::length(direction) > 0.0);
            const Ray ray{origin, glm::normalize(direction)};
            const auto expected = bruteFirst(solids, ray);
            const auto hits = index.pick(ray);
            const auto first = index.first(ray);
            REQUIRE(hits.empty() == !expected);
            REQUIRE(first.has_value() == expected.has_value());
            if (!expected) continue;
            CHECK(hits.front().node == expected->node);
            CHECK(hits.front().side == expected->side);
            CHECK(hits.front().t == doctest::Approx(expected->t).epsilon(1e-8));
            CHECK(first->node == hits.front().node);
            CHECK(first->side == hits.front().side);
            checkPoint(hits.front().point, expected->point);
            checkPoint(hits.front().normal, expected->normal);
        }
        const auto moving = std::get<Handle<Solid>>(*doc.handle(solids[0].id));
        auto geometry = geometry::solidGeometry(*doc.get(moving)->block);
        REQUIRE(geometry);
        translate(*geometry, {1, 0, 0});
        auto tx = doc.begin("corpus single-solid move");
        tx.setSolidGeometry(moving, *geometry);
        const auto changes = tx.commit();
        const auto refitStart = std::chrono::steady_clock::now();
        index.apply(changes);
        const auto refitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - refitStart).count();
        MESSAGE("Pick corpus ", path.filename().string(), ": ", solids.size(), " visible valid solids, ",
                rejected, " skipped; 10000 rays; build ", buildMs, " ms; single-solid refit ", refitMs,
                " ms (release targets: <100 ms / <1 ms; no timing assertions)");
    }
}
