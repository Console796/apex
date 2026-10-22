#include "render/view_math.hpp"

#include <doctest/doctest.h>

using namespace apex;

namespace {

// Projects a view-space point (x right, y down, z forward) and returns ndc (x, y, depth).
Vec3 project(const Mat4& m, Vec3 p) {
    const Vec4 clip = m * Vec4(p, 1.0f);
    return Vec3(clip) / clip.w;
}

} // namespace

TEST_CASE("reversed-Z perspective maps near to 1 and far to 0") {
    const auto finite = render::reversedZPerspective(1.5, 16.0 / 9.0, 0.1, 1000.0);
    CHECK(project(finite, {0, 0, 0.1f}).z == doctest::Approx(1.0).epsilon(1e-5));
    CHECK(project(finite, {0, 0, 1000.0f}).z == doctest::Approx(0.0).epsilon(1e-5));
    CHECK(project(finite, {0, 0, 10.0f}).z < project(finite, {0, 0, 5.0f}).z);

    const auto infinite = render::reversedZPerspective(1.5, 1.0, 0.1);
    CHECK(project(infinite, {0, 0, 0.1f}).z == doctest::Approx(1.0).epsilon(1e-5));
    CHECK(project(infinite, {0, 0, 1e6f}).z == doctest::Approx(0.0).epsilon(1e-6).scale(1.0));
    CHECK(project(infinite, {0, 0, 1e6f}).z > 0.0f);
}

TEST_CASE("perspective frustum edges land on the clip boundary, y down") {
    const double fov = 1.0, aspect = 2.0;
    const auto m = render::reversedZPerspective(fov, aspect, 0.1);
    const float z = 5.0f;
    const float top = z * static_cast<float>(std::tan(fov / 2));
    CHECK(project(m, {0, top, z}).y == doctest::Approx(1.0).epsilon(1e-5));
    CHECK(project(m, {top * 2.0f, 0, z}).x == doctest::Approx(1.0).epsilon(1e-5));
}

TEST_CASE("reversed-Z ortho maps near to 1 and far to 0") {
    const auto m = render::reversedZOrtho(10.0, 5.0, 1.0, 101.0);
    CHECK(project(m, {10, 5, 1}).x == doctest::Approx(1.0));
    CHECK(project(m, {10, 5, 1}).y == doctest::Approx(1.0));
    CHECK(project(m, {0, 0, 1}).z == doctest::Approx(1.0));
    CHECK(project(m, {0, 0, 101}).z == doctest::Approx(0.0).epsilon(1e-6).scale(1.0));
    CHECK(project(m, {0, 0, 51}).z == doctest::Approx(0.5));
}

TEST_CASE("rotation-only view maps Source axes to view space") {
    // Yaw 0, pitch 0: forward +X, right -Y (Source), up +Z.
    const auto basis = angleVectors({0, 0, 0});
    const auto view = render::rotationOnlyView(basis);
    const Vec4 ahead = view * Vec4(10, 0, 0, 1);
    CHECK(ahead.z == doctest::Approx(10.0));
    CHECK(ahead.x == doctest::Approx(0.0).epsilon(1e-6).scale(1.0));
    const Vec4 up = view * Vec4(0, 0, 3, 1);
    CHECK(up.y == doctest::Approx(-3.0)); // View y is down.
    const Vec4 right = view * Vec4(basis.right.x, basis.right.y, basis.right.z, 1);
    CHECK(right.x == doctest::Approx(1.0));
}

TEST_CASE("camera-relative rebasing keeps float precision far from the origin") {
    const DVec3 camera{1.0e7, -2.0e7, 5.0e6};
    const DVec3 point = camera + DVec3{0.25, -0.5, 1.0};
    const Vec3 relative = render::toCameraRelative(point, camera);
    CHECK(relative.x == doctest::Approx(0.25));
    CHECK(relative.y == doctest::Approx(-0.5));
    CHECK(relative.z == doctest::Approx(1.0));
    // Direct narrowing would have lost this: float spacing at 1e7 is 1.
    CHECK(static_cast<float>(point.x) - static_cast<float>(camera.x) != 0.25f);
    const Mat4 model = render::cameraRelativeTranslation(point, camera);
    CHECK(model[3].x == doctest::Approx(0.25));
    CHECK(model[3].w == doctest::Approx(1.0));
}

TEST_CASE("grid camera wrap preserves plane-space microgrid phases at world extremes") {
    for (const double base : {-65536.0, 0.0, 65536.0}) {
        CAPTURE(base);
        const DVec2 projected = render::gridCameraCoordinates(
            {base + 0.0625, base - 0.03125, 128}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0});
        CHECK(projected == DVec2(base + 0.0625, base - 0.03125));
        // Every minor period and every major period (eight times the minor spacing).
        for (double period = 0.125; period <= 8192; period *= 2) {
            CAPTURE(period);
            CHECK(render::wrapGridCoordinates(projected, period) == Vec2(0.0625f, float(period - 0.03125)));
            CHECK(render::wrapGridCoordinates({base, -base}, period) == Vec2(0));
        }
    }
    CHECK(render::wrapGridCoordinates({-0.125, -1024.125}, 0.125) == Vec2(0));
    CHECK(render::wrapGridCoordinates({1024.125, -1024.125}, 8192) == Vec2(1024.125f, 7167.875f));
    CHECK(render::wrapGridCoordinates({-0.000244140625, 65536.000244140625}, 0.125) ==
          Vec2(0.124755859375f, 0.000244140625f));

    // Translation, normal distance and axis signs do not change the plane-space phase.
    const DVec3 origin{60000, -65536, 65536};
    const DVec3 u{0, 1, 0}, v{0, 0, -1};
    const DVec2 wall = render::gridCameraCoordinates(origin + u * -0.0625 + v * 1024.125 + DVec3(128, 0, 0),
                                                    origin, u, v);
    CHECK(wall == DVec2(-0.0625, 1024.125));
    CHECK(render::wrapGridCoordinates(wall, 0.125) == Vec2(0.0625f, 0.0f));
    CHECK(render::wrapGridCoordinates(wall, 1.0) == Vec2(0.9375f, 0.125f));
    const DVec3 slopeU{0.8, 0, -0.6}, slopeV{0, 1, 0}, normal{0.6, 0, 0.8};
    const DVec2 slope = render::gridCameraCoordinates(origin + slopeU * 0.125 + slopeV * -0.25 + normal * 256.0,
                                                     origin, slopeU, slopeV);
    CHECK(slope.x == doctest::Approx(0.125).epsilon(1e-8));
    CHECK(slope.y == doctest::Approx(-0.25).epsilon(1e-8));
}
