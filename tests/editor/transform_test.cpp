#include "fixtures.hpp"
#include "editor/transform.hpp"
#include "geometry/tolerances.hpp"

#include <array>
#include <limits>

using namespace editor_test;

namespace {

const SurfacePlane xy{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {}};

geometry::SolidGeometry box() {
    const auto source = solidBlock(geometry_test::build(geometry_test::boxSides({-13, -5, 3}, {27, 43, 35})));
    auto solid = geometry::solidGeometry(source);
    REQUIRE(solid);
    for (auto& face : solid->poly.faces) {
        face.uv.uaxis = {0.7, -0.2, 0.4};
        face.uv.vaxis = {-0.3, 0.8, -0.6};
        face.uv.shiftU = 12.25;
        face.uv.shiftV = -8.5;
        face.uv.scaleU = 0.25;
        face.uv.scaleV = 0.75;
    }
    return *solid;
}

DVec2 texel(const geometry::FaceUv& uv, DVec3 point) {
    return {glm::dot(uv.uaxis, point) / uv.scaleU + uv.shiftU,
            glm::dot(uv.vaxis, point) / uv.scaleV + uv.shiftV};
}

DVec3 key(const Document& doc, NodeHandle h, std::string_view name) {
    const auto* entry = doc.get(h)->block->find(name);
    REQUIRE(entry);
    const auto value = entry->asVec3();
    REQUIRE(value);
    return {(*value)[0], (*value)[1], (*value)[2]};
}

std::vector<Transform> transforms() {
    const auto frame = DMat3{Transform::rotate({1, 2, 3}, 37).linear()};
    return {Transform::translate({12.5, -7, 3}),
            Transform::rotate({1, 2, -1}, 43, {3, -9, 2}),
            Transform::scale({2, 0.5, 1.25}, frame, {3, -9, 2}),
            Transform::mirror({1, 2, 3}, {3, -9, 2})};
}

} // namespace

TEST_CASE("transform affine composition inverse frames and determinant") {
    const DVec3 p{7, -2, 19}, pivot{2, 3, 4};
    const auto a = Transform::translate({1, 2, 3});
    const auto b = Transform::rotate({0, 0, 1}, 90, pivot);
    checkPoint((a * b).point(p), a.point(b.point(p)));
    checkPoint(b.point(pivot), pivot);
    checkPoint(b.point(pivot + DVec3{1, 0, 0}), pivot + DVec3{0, 1, 0});
    const auto frame = Transform::rotate({0, 0, 1}, 90).linear();
    const auto scaled = Transform::scale({2, 3, 4}, frame, pivot);
    checkPoint(scaled.point(pivot + DVec3{1, 0, 0}), pivot + DVec3{3, 0, 0});
    CHECK_FALSE(scaled.isRigid());
    CHECK(a.isRigid());
    CHECK(b.isRigid());
    CHECK_FALSE(b.reversesOrientation());
    const auto mirror = Transform::mirror({1, 0, 0}, pivot);
    CHECK(mirror.isRigid());
    CHECK(mirror.reversesOrientation());
    checkPoint(mirror.point(pivot + DVec3{4, 5, 6}), pivot + DVec3{-4, 5, 6});
    for (const auto& transform : transforms()) {
        checkPoint(transform.inverse().point(transform.point(p)), p);
        checkPoint(DVec3{transform.matrix() * DVec4{p, 1}}, transform.point(p));
    }
    CHECK_THROWS_AS(Transform::scale({1, 0, 1}), std::invalid_argument);
    CHECK_THROWS_AS(Transform::rotate({0, 0, 0}, 30), std::invalid_argument);
    CHECK_THROWS_AS(Transform::mirror({0, 0, 0}), std::invalid_argument);
    CHECK_THROWS_AS(Transform::translate({std::numeric_limits<double>::infinity(), 0, 0}), std::invalid_argument);
}

TEST_CASE("transform D8 translate absolute corner relative delta and suspended") {
    const AABB bounds{{3, 5, 7}, {19, 25, 39}};
    SnapSettings settings;
    settings.gridSize = 8;
    checkPoint(snapTranslate({5, 6, 2}, bounds, bounds.min, xy, settings), {5, 3, 2});
    checkPoint(snapTranslate({5, 6, 2}, bounds, bounds.max, xy, settings), {5, 7, 2});
    settings.mode = SnapMode::Relative;
    checkPoint(snapTranslate({5, 6, 2}, bounds, bounds.min, xy, settings), {8, 8, 2});
    checkPoint(snapTranslate({-5, -6, -2}, bounds, bounds.max, xy, settings), {-8, -8, -2});
    for (const auto mode : {SnapMode::Absolute, SnapMode::Relative}) {
        settings.mode = mode;
        settings.suspended = true;
        checkPoint(snapTranslate({5.25, -6.1, 2}, bounds, bounds.max, xy, settings), {5.25, -6.1, 2});
    }
    // Grid coordinates are relative to the surface origin, including on sloped planes.
    const auto rotation = Transform::rotate({1, 1, 0}, 30);
    auto sloped = xy;
    sloped.origin = {5, 2, -8};
    sloped.uAxis = rotation.linear() * xy.uAxis;
    sloped.vAxis = rotation.linear() * xy.vAxis;
    sloped.normal = rotation.linear() * xy.normal;
    settings.suspended = false;
    settings.mode = SnapMode::Absolute;
    const auto delta = snapTranslate({2, 3, 4}, bounds, bounds.min, sloped, settings);
    const auto final = bounds.min + delta - sloped.origin;
    for (const auto axis : {sloped.uAxis, sloped.vAxis})
        CHECK(glm::dot(final, axis) / 8.0 == doctest::Approx(std::round(glm::dot(final, axis) / 8.0)));
    CHECK(glm::dot(delta, sloped.normal) == doctest::Approx(glm::dot(DVec3{2, 3, 4}, sloped.normal)));
}

TEST_CASE("transform D8 rotate yaw seven prop absolute relative fine and suspended") {
    SnapSettings settings;
    CHECK(7 + snapRotate(10, 7, settings) == 15);
    CHECK(7 + snapRotate(21, 7, settings) == 30);
    CHECK(snapRotate(10, 0, settings) == 15); // Local brush frame starts at zero.
    settings.mode = SnapMode::Relative;
    CHECK(7 + snapRotate(10, 7, settings) == 22);
    CHECK(snapRotate(-10, 7, settings) == -15);
    CHECK(snapRotate(10, 0, settings) == 15);
    for (const auto mode : {SnapMode::Absolute, SnapMode::Relative}) {
        settings.mode = mode;
        settings.fine = true;
        CHECK(snapRotate(2.7, 7, settings) == 3);
        settings.suspended = true;
        CHECK(snapRotate(2.7, 7, settings) == 2.7);
        settings.suspended = false;
    }
    settings.fine = false;
    settings.angleSnap = 30;
    CHECK(snapRotate(22, 0, settings) == 30);
}

TEST_CASE("transform D8 scale absolute bounds relative size and suspended") {
    const AABB bounds{{3, 5, 7}, {23, 35, 47}};
    SnapSettings settings;
    settings.gridSize = 8;
    const DVec3 factors{1.3, 1.41, 1};
    const auto absolute = snapScale(factors, bounds, bounds.min, settings);
    checkPoint(absolute, {29.0 / 20.0, 43.0 / 30.0, 1});
    const auto endpoint = bounds.min + (bounds.max - bounds.min) * absolute;
    CHECK(endpoint.x == 32);
    CHECK(endpoint.y == 48);
    CHECK(endpoint.z == 47); // Inactive axis stays untouched.
    settings.mode = SnapMode::Relative;
    const auto relative = snapScale(factors, bounds, bounds.min, settings);
    checkPoint(relative, {1.4, 1.5333333333333333, 1});
    for (int i = 0; i < 2; ++i) {
        const double change = (bounds.max[i] - bounds.min[i]) * (relative[i] - 1);
        CHECK(change / 8 == doctest::Approx(std::round(change / 8)));
    }
    // On-grid minimum and maximum remain on-grid with an anchored scale.
    for (const auto mode : {SnapMode::Absolute, SnapMode::Relative}) {
        settings.mode = mode;
        const AABB onGrid{{8, 16, 24}, {40, 64, 88}};
        const auto result = snapScale({1.3, 0.7, 1.1}, onGrid, onGrid.min, settings);
        const auto max = onGrid.min + (onGrid.max - onGrid.min) * result;
        for (int i = 0; i < 3; ++i) CHECK(max[i] / 8 == doctest::Approx(std::round(max[i] / 8)));
        settings.suspended = true;
        CHECK(snapScale(factors, bounds, bounds.min, settings) == factors);
        settings.suspended = false;
    }
    // Scale from max: the farther, minimum endpoint is the moving one.
    settings.mode = SnapMode::Absolute;
    const auto negativeReach = snapScale({1.3, 1, 1}, bounds, bounds.max, settings);
    CHECK(bounds.max.x + (bounds.min.x - bounds.max.x) * negativeReach.x == doctest::Approx(0));
}

TEST_CASE("transform texture lock preserves each vertex texel for affine transforms") {
    const auto source = box();
    for (const auto& transform : transforms()) {
        const auto locked = applyToSolid(source, transform, true);
        const auto unlocked = applyToSolid(source, transform, false);
        REQUIRE(locked);
        REQUIRE(unlocked);
        CHECK(locked->poly.isValid());
        CHECK(locked->sides.size() == source.sides.size());
        for (std::size_t f = 0; f < source.poly.faces.size(); ++f) {
            const auto& oldFace = source.poly.faces[f];
            const auto& newFace = locked->poly.faces[f];
            CHECK(oldFace.sideId == newFace.sideId);
            for (const auto i : oldFace.vertices) {
                CHECK(glm::length(texel(oldFace.uv, source.poly.vertices[i]) -
                                  texel(newFace.uv, locked->poly.vertices[i])) < 1e-9);
            }
            CHECK(unlocked->poly.faces[f].uv.uaxis == oldFace.uv.uaxis);
            CHECK(unlocked->poly.faces[f].uv.vaxis == oldFace.uv.vaxis);
            CHECK(unlocked->poly.faces[f].uv.shiftU == oldFace.uv.shiftU);
            CHECK(unlocked->poly.faces[f].uv.shiftV == oldFace.uv.shiftV);
            const auto side = std::ranges::find(locked->sides, newFace.sideId, &geometry::PointDataSide::sideId);
            REQUIRE(side != locked->sides.end());
            CHECK(side->points.size() == newFace.vertices.size());
            CHECK(side->uv.uaxis == newFace.uv.uaxis);
        }
    }
}

TEST_CASE("transform mirrors reverse loops and rotation inverse stays within point merge epsilon") {
    const auto source = box();
    for (const auto normal : {DVec3{1, 0, 0}, DVec3{0, 1, 0}, DVec3{0, 0, 1}, DVec3{1, 2, 3}}) {
        const auto flipped = applyToSolid(source, Transform::mirror(normal));
        REQUIRE(flipped);
        CHECK(flipped->poly.validate().empty());
        for (std::size_t f = 0; f < source.poly.faces.size(); ++f) {
            auto loop = source.poly.faces[f].vertices;
            std::ranges::reverse(loop);
            CHECK(flipped->poly.faces[f].vertices == loop);
        }
    }
    const auto rotation = Transform::rotate({1, 2, 3}, 137, {65536, -65536, 100});
    const auto rotated = applyToSolid(source, rotation);
    REQUIRE(rotated);
    const auto restored = applyToSolid(*rotated, rotation.inverse());
    REQUIRE(restored);
    for (std::size_t i = 0; i < source.poly.vertices.size(); ++i)
        CHECK(glm::length(source.poly.vertices[i] - restored->poly.vertices[i]) < geometry::kPointMergeEps);
    auto invalid = source;
    invalid.poly.vertices[0] = invalid.poly.vertices[1];
    CHECK_FALSE(applyToSolid(invalid, rotation));
}

TEST_CASE("transform entity Source pitch yaw roll known Hammer convention results") {
    struct Case { DVec3 start, axis; double degrees; DVec3 expected; };
    // Source basis: +pitch sends +X toward -Z; +yaw sends +X toward +Y;
    // +roll sends +Z toward -Y. Compose world rotations on those basis columns.
    const std::array cases{
        Case{{0, 0, 0}, {0, 0, 1}, 90, {0, 90, 0}},
        Case{{0, 0, 0}, {0, 1, 0}, 30, {30, 0, 0}},
        Case{{0, 0, 0}, {1, 0, 0}, 30, {0, 0, 30}},
        // Global yaw rotates forward and up together: pitched/rolled values survive.
        Case{{30, 20, 40}, {0, 0, 1}, 90, {30, 110, 40}},
        // At yaw 90, forward=+Y and up=+Z. World +X rotates forward toward
        // +Z (pitch=-30), leaving yaw=90 and roll=0.
        Case{{0, 90, 0}, {1, 0, 0}, 30, {-30, 90, 0}},
        // With roll=90, up=-Y; world +Y rotation pitches forward but fixes up.
        Case{{0, 0, 90}, {0, 1, 0}, 30, {30, 0, 90}},
        // Vertical forward=-Z, up=+X: the singular representation chooses yaw=0.
        Case{{0, 0, 0}, {0, 1, 0}, 90, {90, 0, 0}}
    };
    for (const auto& c : cases) {
        CAPTURE(c.start.x);
        CAPTURE(c.start.y);
        CAPTURE(c.start.z);
        Document doc(parsed());
        std::ostringstream text;
        text << "entity { \"classname\" \"prop_dynamic\" \"origin\" \"1 2 3\" \"angles\" \""
             << c.start.x << ' ' << c.start.y << ' ' << c.start.z << "\" }";
        const auto entity = addBlock(doc, text.str());
        const auto transform = Transform::rotate(c.axis, c.degrees);
        const auto result = applyToEntity(doc, id(doc, entity), transform);
        REQUIRE(result);
        REQUIRE(result->origin);
        REQUIRE(result->angles);
        checkPoint(*result->origin, transform.point({1, 2, 3}));
        checkPoint(*result->angles, c.expected);
    }
    Document doc(parsed());
    const auto entity = addBlock(doc, "entity { \"classname\" \"info_target\" }");
    const auto missing = applyToEntity(doc, id(doc, entity), Transform::translate({3, 4, 5}));
    REQUIRE(missing);
    checkPoint(*missing->origin, {3, 4, 5});
    checkPoint(*missing->angles, {0, 0, 0});
}

TEST_CASE("transform commit preview single undo byte restoration and brush origin") {
    for (const auto& transform : transforms()) {
        for (const bool hasOrigin : {false, true}) {
            Document doc(parsed("// retained source trivia\nworld { \"id\" \"1\" \"classname\" \"worldspawn\" }\n"));
            const auto brush = addBlock(doc, hasOrigin
                ? "entity { \"classname\" \"func_door\" \"origin\" \"3 7 11\" \"angles\" \"0 5 0\" }"
                : "entity { \"classname\" \"func_detail\" }");
            const auto solid = addBox(doc, {-13, -5, 3}, {27, 43, 35}, brush);
            const auto point = addBlock(doc, "entity { \"classname\" \"info_target\" \"origin\" \"2 4 8\" \"angles\" \"30 20 40\" }");
            const auto old = geometry::solidGeometry(*doc.get(solid)->block);
            REQUIRE(old);
            const auto preview = transformedVertices(*old, transform);
            const auto before = doc.write();
            REQUIRE(before);
            const auto count = doc.undoCount();
            int notifications = 0;
            doc.observe([&](const ChangeSet&) { ++notifications; });
            // Overlapping selection must transform each solid once.
            const std::array selection{id(doc, brush), id(doc, solid), id(doc, point), id(doc, brush)};
            REQUIRE(commitTransform(doc, selection, transform, {}, "Transform"));
            CHECK(doc.undoCount() == count + 1);
            CHECK(notifications == 1);
            const auto committed = geometry::solidGeometry(*doc.get(solid)->block);
            REQUIRE(committed);
            CHECK(committed->poly.isValid());
            // Serialization may reorder the shared vertex pool; compare positions by membership.
            for (const auto vertex : preview) {
                CHECK(std::ranges::any_of(committed->poly.vertices, [&](DVec3 p) {
                    return glm::length(p - vertex) < geometry::kPointMergeEps;
                }));
            }
            if (hasOrigin) {
                checkPoint(key(doc, brush, "origin"), transform.point({3, 7, 11}));
                checkPoint(key(doc, brush, "angles"), {0, 5, 0});
            } else CHECK_FALSE(doc.get(brush)->block->find("origin"));
            checkPoint(key(doc, point, "origin"), transform.point({2, 4, 8}));
            const auto after = doc.write();
            REQUIRE(after);
            CHECK(doc.undo());
            CHECK(doc.write().value() == before.value());
            CHECK(doc.redo());
            CHECK(doc.write().value() == after.value());
        }
    }
}

TEST_CASE("transform commit preflight failure leaves bytes and history untouched") {
    Document doc(parsed());
    const auto good = addBox(doc, {0, 0, 0}, {32, 32, 32});
    const auto bad = addBlock(doc, "solid { side { \"id\" \"10\" } }");
    const std::array selection{id(doc, good), id(doc, bad)};
    const auto before = doc.write();
    const auto count = doc.undoCount();
    CHECK_FALSE(commitTransform(doc, selection, Transform::translate({32, 0, 0}), {}, "Move"));
    CHECK(doc.write().value() == before.value());
    CHECK(doc.undoCount() == count);
    const std::array missing{NodeId{99999}};
    CHECK_FALSE(commitTransform(doc, missing, Transform{}, {}, "Missing"));
    CHECK(doc.write().value() == before.value());
}

TEST_CASE("transform nudge uses resolved work plane and viewport fallback") {
    WorkPlaneState state;
    checkPoint(nudgeDelta(state, xy, false, 8), {8, 0, 0});
    state.pinAxisPlane(Axis::X, {1, 2, 3});
    checkPoint(nudgeDelta(state, xy, false, 16), {0, 16, 0});
    checkPoint(nudgeDelta(state, xy, true, 16, -1), {0, 0, -16});
    state.unpin();
    state.setHovered(xy);
    checkPoint(nudgeDelta(state, xy, true, 0.125), {0, 0.125, 0});
}

TEST_CASE("transform consecutive point nudges each have their own undo entry") {
    Document doc(parsed());
    const auto entity = addBlock(doc, "entity { \"classname\" \"info_target\" \"origin\" \"0 0 0\" \"angles\" \"0 0 0\" }");
    const std::array selection{id(doc, entity)};
    const auto before = doc.write().value();
    const auto count = doc.undoCount();
    REQUIRE(commitTransform(doc, selection, Transform::translate({8, 0, 0}), {}, "Nudge"));
    const auto first = doc.write().value();
    REQUIRE(commitTransform(doc, selection, Transform::translate({8, 0, 0}), {}, "Nudge"));
    CHECK(doc.undoCount() == count + 2);
    checkPoint(key(doc, entity, "origin"), {16, 0, 0});
    REQUIRE(doc.undo());
    CHECK(doc.write().value() == first);
    REQUIRE(doc.undo());
    CHECK(doc.write().value() == before);
}

TEST_CASE("transform document write rejection rolls back the whole gesture") {
    Document doc(parsed());
    const auto solid = addBox(doc, {0, 0, 0}, {32, 32, 32});
    const auto entity = addBlock(doc, "entity { \"classname\" \"info_target\" \"origin\" \"0 0 0\" \"origin\" \"1 2 3\" }");
    const std::array selection{id(doc, solid), id(doc, entity)};
    const auto before = doc.write().value();
    const auto count = doc.undoCount();
    int notifications = 0;
    doc.observe([&](const ChangeSet&) { ++notifications; });
    CHECK_FALSE(commitTransform(doc, selection, Transform::translate({8, 0, 0}), {}, "Move"));
    CHECK(doc.write().value() == before);
    CHECK(doc.undoCount() == count);
    CHECK(notifications == 0);
}
