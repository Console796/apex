#include "fixtures.hpp"
#include "geometry/face_uv.hpp"

using namespace geometry_test;
namespace {
// Justify and wrap keep shifts within one texture.
double within(double shift, int size) { return std::fmod(shift, size); }
void closeUv(const FaceUv& a, const FaceUv& b) {
    checkVector(a.uaxis, b.uaxis); checkVector(a.vaxis, b.vaxis);
    CHECK(a.shiftU == doctest::Approx(b.shiftU)); CHECK(a.shiftV == doctest::Approx(b.shiftV));
    CHECK(a.scaleU == doctest::Approx(b.scaleU)); CHECK(a.scaleV == doctest::Approx(b.scaleV));
}
}
TEST_CASE("UV justify canonical bounds on axial sloped and rotated textures") {
    for (bool sloped : {false, true}) for (bool rotated : {false, true}) {
        CAPTURE(sloped); CAPTURE(rotated);
        std::vector<DVec3> points{{10, -20, 0}, {30, -20, 0}, {30, -60, 0}, {10, -60, 0}};
        if (sloped) for (auto& p : points) p.z = p.x;
        FaceUv uv;
        if (rotated) { uv.uaxis = {0, 1, 0}; uv.vaxis = {1, 0, 0}; }
        uv.shiftU = 7; uv.shiftV = -9;
        const double minU = rotated ? -60 : 10, maxU = rotated ? -20 : 30;
        const double minV = rotated ? 10 : 20, maxV = rotated ? 30 : 60;
        const auto left = justify(uv, points, Justify::Left, 100, 80);
        CHECK(left.shiftU == doctest::Approx(within(-minU, 100))); CHECK(left.shiftV == -9);
        const auto right = justify(uv, points, Justify::Right, 100, 80);
        CHECK(right.shiftU == doctest::Approx(within(100 - maxU, 100))); CHECK(right.shiftV == -9);
        const auto top = justify(uv, points, Justify::Top, 100, 80);
        CHECK(top.shiftV == doctest::Approx(within(-minV, 80))); CHECK(top.shiftU == 7);
        const auto bottom = justify(uv, points, Justify::Bottom, 100, 80);
        CHECK(bottom.shiftV == doctest::Approx(within(80 - maxV, 80))); CHECK(bottom.shiftU == 7);
        const auto centre = justify(uv, points, Justify::Center, 100, 80);
        CHECK(centre.shiftU == doctest::Approx(within(50 - (minU + maxU) / 2, 100)));
        CHECK(centre.shiftV == doctest::Approx(within(40 - (minV + maxV) / 2, 80)));
        const auto fit = justify(uv, points, Justify::Fit, 100, 80);
        CHECK(fit.scaleU == doctest::Approx((maxU - minU) / 100));
        CHECK(fit.scaleV == doctest::Approx((maxV - minV) / 80));
        CHECK(fit.shiftU == doctest::Approx(within(-minU / fit.scaleU, 100)));
        CHECK(fit.shiftV == doctest::Approx(within(-minV / fit.scaleV, 80)));
        uv.scaleU = -2; uv.scaleV = -3;
        const auto negative = justify(uv, points, Justify::Fit, 100, 80);
        CHECK(negative.scaleU == doctest::Approx(-(maxU - minU) / 100));
        CHECK(negative.scaleV == doctest::Approx(-(maxV - minV) / 80));
    }
}
TEST_CASE("UV world face alignment and rotation round trip ignore stored metadata") {
    for (const auto n : {DVec3{1, 0, 0}, DVec3{-1, 0, 0}, DVec3{0, 1, 0}, DVec3{0, -1, 0}, DVec3{0, 0, 1}, DVec3{0, 0, -1}, glm::normalize(DVec3{1, 2, 3})}) {
        Face face; face.plane = {n, 0};
        const auto uv = alignWorld({}, n);
        closeUv(uv, generateUv(face, 128, 64));
        CHECK(deriveRotation(uv, n) == 0);
        const auto planar = alignFace(uv, n);
        CHECK(glm::dot(planar.uaxis, n) == doctest::Approx(0).epsilon(1e-10));
        CHECK(glm::dot(planar.vaxis, n) == doctest::Approx(0).epsilon(1e-10));
        CHECK(glm::length(planar.uaxis) == doctest::Approx(1)); CHECK(glm::length(planar.vaxis) == doctest::Approx(1));
        for (double a : {0., 15., 90., 180., 270., 359., -45., 450., 90.0000000001}) {
            CAPTURE(n.x); CAPTURE(n.y); CAPTURE(n.z); CAPTURE(a);
            const auto turned = rotateUv(uv, n, a);
            CHECK(deriveRotation(turned, n) == doctest::Approx(std::fmod(a + 720, 360)).epsilon(1e-8));
            closeUv(rotateUv(turned, n, -a), uv);
        }
    }
    CHECK(deriveRotation(rotateUv({}, {0, 0, 1}, 90.0000000001), {0, 0, 1}) == 90);
    checkVector(rotateUv({}, {0, 0, 1}, 90).uaxis, {0, 1, 0});
}
TEST_CASE("UV wrap preserves texels along 90 degree 45 degree and coplanar shared edges") {
    const DVec3 a{32, 16, 10}, b{32, 16, 90};
    for (auto n : {DVec3{0, 1, 0}, glm::normalize(DVec3{1, 1, 0}), DVec3{1, 0, 0}}) {
        Face from, to;
        from.uv = rotateUv(alignWorld({}, {1, 0, 0}), {1, 0, 0}, 37);
        from.uv.scaleU = -.25; from.uv.scaleV = .5;
        from.uv.shiftU = 23; from.uv.shiftV = -7;
        to.uv = wrapUv(from.uv, {1, 0, 0}, n, a, b, 128, 64);
        CHECK(std::abs(glm::dot(to.uv.uaxis, n)) < 1e-10);
        CHECK(std::abs(glm::dot(to.uv.vaxis, n)) < 1e-10);
        for (double t : {0., .2, .7, 1.}) {
            const auto p = a + (b - a) * t;
            // The texture repeats: whole textures apart is the same texel.
            const auto d = uvAt(from, p, 128, 64) - uvAt(to, p, 128, 64);
            CHECK(glm::length(d - glm::round(d)) < 1e-10);
        }
        if (n == DVec3{1, 0, 0}) closeUv(to.uv, from.uv);
    }
}
TEST_CASE("UV functions reject invalid dimensions normals values and bounds") {
    const std::vector<DVec3> points{{0, 0, 0}, {1, 1, 0}};
    CHECK_THROWS_AS(justify({}, points, Justify::Left, 0, 1), std::invalid_argument);
    CHECK_THROWS_AS(justify({}, {}, Justify::Fit, 1, 1), std::invalid_argument);
    CHECK_THROWS_AS(alignWorld({}, {0, 0, 0}), std::invalid_argument);
    CHECK_THROWS_AS(alignFace({}, {0, 0, 2}), std::invalid_argument);
    CHECK_THROWS_AS(rotateUv({}, {0, 0, 1}, INFINITY), std::invalid_argument);
    FaceUv bad; bad.scaleU = 0;
    CHECK_THROWS_AS(deriveRotation(bad, {0, 0, 1}), std::invalid_argument);
    CHECK_THROWS_AS(wrapUv({}, {1, 0, 0}, {0, 1, 0}, {0, 0, 0}, {1, 0, 0}, 1, 1), std::invalid_argument);
}
TEST_CASE("UV pivot operations preserve the texel and alignment cells report their justify") {
    const std::vector<DVec3> points{{-20, -12, 0}, {20, -12, 0}, {20, 12, 0}, {-20, 12, 0}};
    const auto sloped = glm::normalize(DVec3{1, 1, 1});
    for (const auto normal : {DVec3{0, 0, 1}, sloped}) {
        auto uv = alignFace({}, normal);
        uv.shiftU = 23; uv.shiftV = -19; uv.scaleU = -.5; uv.scaleV = .75;
        const auto pivot = points.front();
        Face face; face.uv = uv;
        const auto before = uvAt(face, pivot, 128, 64);
        const auto rotated = rotateUvAbout(uv, normal, pivot, 73);
        face.uv = rotated;
        CHECK(glm::length(uvAt(face, pivot, 128, 64) - before) < 1e-12);
        const auto scaled = scaleUvAbout(uv, pivot, 1.7, .6);
        face.uv = scaled;
        CHECK(glm::length(uvAt(face, pivot, 128, 64) - before) < 1e-12);
        CHECK_THROWS_AS(scaleUvAbout(uv, pivot, 0, 1), std::invalid_argument);
        const auto shifted = translateUv(uv, 333, -71, 128, 64);
        CHECK(std::abs(shifted.shiftU) < 128); CHECK(std::abs(shifted.shiftV) < 64);
        face.uv = shifted;
        const DVec2 delta{333.0 / 128, -71.0 / 64};
        const auto change = uvAt(face, pivot, 128, 64) - before - delta;
        CHECK(std::abs(std::remainder(change.x, 1.0)) < 1e-10);
        CHECK(std::abs(std::remainder(change.y, 1.0)) < 1e-10);
        auto wide = uv; wide.shiftV = 100;
        CHECK(translateUv(wide, 3, 0, 128, 64).shiftV == 100); // The unmoved axis isn't rewrapped.
    }
    const std::pair<HorizontalAlignment, VerticalAlignment> cells[] = {
        {HorizontalAlignment::Left, VerticalAlignment::Top},
        {HorizontalAlignment::Center, VerticalAlignment::Top},
        {HorizontalAlignment::Right, VerticalAlignment::Top},
        {HorizontalAlignment::Left, VerticalAlignment::Center},
        {HorizontalAlignment::Center, VerticalAlignment::Center},
        {HorizontalAlignment::Right, VerticalAlignment::Center},
        {HorizontalAlignment::Left, VerticalAlignment::Bottom},
        {HorizontalAlignment::Center, VerticalAlignment::Bottom},
        {HorizontalAlignment::Right, VerticalAlignment::Bottom}};
    for (const auto [horizontal, vertical] : cells) {
        const auto aligned = justify2({}, points, horizontal, vertical, 128, 64);
        const auto found = alignmentOf(aligned, points, 128, 64);
        CHECK(found.horizontal == horizontal); CHECK(found.vertical == vertical);
    }
}
TEST_CASE("UV projected and matched extrapolation preserve their mapping rules") {
    auto source = rotateUv(alignWorld({}, {0, 0, 1}), {0, 0, 1}, 31);
    source.shiftU = 19; source.shiftV = -7; source.scaleU = -.5; source.scaleV = 1.25;
    const auto targetNormal = glm::normalize(DVec3{1, 1, 0});
    const auto projected = projectUv(source, glm::normalize(DVec3{1, 1, 1}));
    REQUIRE(projected);
    closeUv(*projected, source);
    Face from, to; from.uv = source; to.uv = *projected;
    const DVec3 point{23, -17, 4};
    CHECK(glm::length(uvAt(from, point, 128, 64) - uvAt(to, point, 128, 64)) < 1e-12);
    CHECK_FALSE(projectUv(source, source.uaxis));
    // A vertical wall from a floor: each axis projects, but both onto the same line.
    CHECK_FALSE(projectUv(source, targetNormal));

    const auto matched = matchUv(source, {0, 0, 1}, targetNormal);
    CHECK(deriveRotation(matched, targetNormal) == doctest::Approx(deriveRotation(source, {0, 0, 1})));
    CHECK(matched.shiftU == source.shiftU); CHECK(matched.shiftV == source.shiftV);
    CHECK(matched.scaleU == source.scaleU); CHECK(matched.scaleV == source.scaleV);
    CHECK(std::abs(glm::dot(matched.uaxis, targetNormal)) < 1e-10);
    CHECK(std::abs(glm::dot(matched.vaxis, targetNormal)) < 1e-10);
}
TEST_CASE("Face plane frame is orthonormal and independent of texture axes") {
    const auto normal = glm::normalize(DVec3{1, 2, 3});
    std::vector<DVec3> points{{0, 0, 0}, {2, 0, -2.0 / 3}, {2, 3, -8.0 / 3}, {0, 3, -2}};
    const auto frame = planeFrame(normal, points);
    CHECK(glm::length(frame.x) == doctest::Approx(1));
    CHECK(glm::length(frame.y) == doctest::Approx(1));
    CHECK(glm::dot(frame.x, frame.y) == doctest::Approx(0).epsilon(1e-10));
    CHECK(glm::dot(frame.x, normal) == doctest::Approx(0).epsilon(1e-10));
    CHECK(glm::dot(frame.y, normal) == doctest::Approx(0).epsilon(1e-10));
    CHECK(glm::length(frame.origin - DVec3{1, 1.5, -4.0 / 3}) < 1e-10);
    // Seen from outside on every side, so the view is never mirrored.
    for (const auto n : {DVec3{0, 0, 1}, DVec3{0, 0, -1}, DVec3{1, 0, 0}, DVec3{-1, 0, 0}, DVec3{0, 1, 0}, DVec3{0, -1, 0}, normal}) {
        const auto f = planeFrame(n, points);
        CHECK(glm::length(glm::cross(f.x, f.y) + n) < 1e-10);
    }
}
