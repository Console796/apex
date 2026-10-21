#include "core/math.hpp"

#include <doctest/doctest.h>

#include <array>
#include <type_traits>

namespace {

void checkVector(apex::DVec3 actual, apex::DVec3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-12).scale(1.0));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-12).scale(1.0));
    CHECK(actual.z == doctest::Approx(expected.z).epsilon(1e-12).scale(1.0));
}

} // namespace

TEST_CASE("Source angle known bases follow pitch yaw roll conventions") {
    const auto identity = apex::angleVectors({0.0, 0.0, 0.0});
    checkVector(identity.forward, {1.0, 0.0, 0.0});
    checkVector(identity.right, {0.0, -1.0, 0.0});
    checkVector(identity.up, {0.0, 0.0, 1.0});
    const auto yaw = apex::angleVectors({0.0, 90.0, 0.0});
    checkVector(yaw.forward, {0.0, 1.0, 0.0});
    checkVector(yaw.right, {1.0, 0.0, 0.0});
    checkVector(yaw.up, {0.0, 0.0, 1.0});
    const auto pitch = apex::angleVectors({90.0, 0.0, 0.0});
    checkVector(pitch.forward, {0.0, 0.0, -1.0});
    checkVector(pitch.right, {0.0, -1.0, 0.0});
    checkVector(pitch.up, {1.0, 0.0, 0.0});
    const auto roll = apex::angleVectors({0.0, 0.0, 90.0});
    checkVector(roll.forward, {1.0, 0.0, 0.0});
    checkVector(roll.right, {0.0, 0.0, -1.0});
    checkVector(roll.up, {0.0, -1.0, 0.0});
    const auto combined = apex::angleVectors({30.0, 45.0, 60.0});
    checkVector(combined.forward, {0.6123724356957945, 0.6123724356957945, -0.5});
    checkVector(combined.right, {0.0473671727453765, -0.6597396084411711, -0.75});
    checkVector(combined.up, {0.7891491309924314, -0.4355957403991576, 0.4330127018922193});
}

TEST_CASE("Source angle basis round trips include singularities and wrapped angles") {
    constexpr std::array pitches{-180.0, -100.0, -90.0, -89.999, -30.0, 0.0,
                                25.0, 89.999, 90.0, 120.0, 360.0};
    constexpr std::array yaws{-270.0, -45.0, 0.0, 65.0, 180.0, 450.0};
    constexpr std::array rolls{-180.0, -70.0, 0.0, 35.0, 90.0, 540.0};
    for (double pitch : pitches) {
        for (double yaw : yaws) {
            for (double roll : rolls) {
                CAPTURE(pitch);
                CAPTURE(yaw);
                CAPTURE(roll);
                const auto basis = apex::basisFromAngles({pitch, yaw, roll});
                // The inverse also accepts scaled vectors, with a component along forward in up.
                const auto angles = apex::anglesFromForward(basis[0] * 3.0,
                                                           basis[2] * 2.0 + basis[0]);
                const auto restored = apex::basisFromAngles(angles);
                for (int column = 0; column < 3; ++column) {
                    checkVector(restored[column], basis[column]);
                    CHECK(glm::length(basis[column]) == doctest::Approx(1.0));
                }
                CHECK(glm::dot(basis[0], basis[1]) == doctest::Approx(0.0).epsilon(1e-12));
                CHECK(glm::dot(basis[0], basis[2]) == doctest::Approx(0.0).epsilon(1e-12));
                checkVector(glm::cross(basis[0], basis[2]), basis[1]);
            }
        }
    }
    const auto angles = apex::anglesFromForward(apex::angleVectors({23.0, -57.0, 42.0}).forward,
                                               apex::angleVectors({23.0, -57.0, 42.0}).up);
    checkVector(angles, {23.0, -57.0, 42.0});
}

TEST_CASE("planes use double precision and point winding") {
    static_assert(std::is_same_v<apex::DVec3::value_type, double>);
    const auto plane = apex::Plane::fromPoints({0.0, 0.0, 2.0}, {3.0, 0.0, 2.0}, {0.0, 4.0, 2.0});
    checkVector(plane.normal, {0.0, 0.0, 1.0});
    CHECK(plane.dist == 2.0);
    CHECK(plane.signedDistance({5.0, 8.0, 5.0}) == 3.0);
    CHECK(plane.signedDistance({0.0, 0.0, -1.0}) == -3.0);
    const auto reversed =
        apex::Plane::fromPoints({0.0, 0.0, 2.0}, {0.0, 4.0, 2.0}, {3.0, 0.0, 2.0});
    CHECK(reversed.signedDistance({5.0, 8.0, 5.0}) == -3.0);
    const auto tilted = apex::Plane::fromPoints({1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0});
    CHECK(glm::length(tilted.normal) == doctest::Approx(1.0));
    CHECK(tilted.signedDistance({1.0, 0.0, 0.0}) == doctest::Approx(0.0));
}

TEST_CASE("AABBs expand from empty and include touching boundaries") {
    apex::AABB bounds;
    CHECK(bounds.empty());
    CHECK_FALSE(bounds.contains(apex::DVec3{0.0}));
    CHECK_FALSE(bounds.intersects(bounds));
    checkVector(bounds.center(), {0.0, 0.0, 0.0});
    checkVector(bounds.extents(), {0.0, 0.0, 0.0});
    bounds.expand(apex::AABB{});
    CHECK(bounds.empty());
    bounds.expand(apex::DVec3{2.0, 4.0, 6.0});
    CHECK_FALSE(bounds.empty());
    CHECK(bounds.contains(apex::DVec3{2.0, 4.0, 6.0}));
    bounds.expand(apex::DVec3{-2.0, 0.0, 2.0});
    checkVector(bounds.center(), {0.0, 2.0, 4.0});
    checkVector(bounds.extents(), {2.0, 2.0, 2.0});
    CHECK(bounds.contains(apex::DVec3{0.0, 2.0, 3.0}));
    CHECK_FALSE(bounds.contains(apex::DVec3{3.0, 2.0, 3.0}));
    CHECK(bounds.contains(bounds));
    CHECK_FALSE(bounds.contains(apex::AABB{}));
    const apex::AABB touching{{2.0, 4.0, 6.0}, {3.0, 5.0, 7.0}};
    CHECK(bounds.intersects(touching));
    CHECK(touching.intersects(bounds));
    CHECK_FALSE(bounds.intersects(apex::AABB{{3.0, 5.0, 7.0}, {4.0, 6.0, 8.0}}));
    bounds.expand(touching);
    CHECK(bounds.contains(touching));
    checkVector(bounds.max, {3.0, 5.0, 7.0});
}

TEST_CASE("rays evaluate their double precision parameter") {
    const apex::Ray ray{{65536.0, 2.0, -4.0}, {0.5, -1.0, 2.0}};
    checkVector(ray.at(0.0), ray.origin);
    checkVector(ray.at(2.0), {65537.0, 0.0, 0.0});
    checkVector(ray.at(-2.0), {65535.0, 4.0, -8.0});
}
