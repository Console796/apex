#include <doctest/doctest.h>

#include "viewport/pick_math.hpp"

#include <algorithm>

using namespace apex;
using namespace apex::viewport;

TEST_CASE("pixelRay matches renderer projection for perspective and ortho") {
    Camera camera;
    camera.position = {10000, -500, 37};
    camera.angles = {23, 67, 12};
    for (const auto kind : {ProjectionKind::Perspective, ProjectionKind::Orthographic}) {
        camera.projection = kind;
        const auto centre = pixelRay(camera, 2, 400, 200, 800, 400);
        CHECK(glm::length(centre.dir - camera.forward()) < 1e-6);
        for (const DVec2 pixel : {DVec2{0, 0}, DVec2{123, 287}, DVec2{800, 400}}) {
            const auto ray = pixelRay(camera, 2, pixel.x, pixel.y, 800, 400);
            const auto point = ray.at(camera.isOrtho() ? 66000 : 500);
            const auto clip = DMat4(camera.projectionMatrix(2)) * DMat4(camera.viewMatrix()) *
                              DVec4(point - camera.position, 1);
            CHECK((clip.x / clip.w + 1) * 400 == doctest::Approx(pixel.x).epsilon(1e-6));
            CHECK((clip.y / clip.w + 1) * 200 == doctest::Approx(pixel.y).epsilon(1e-6));
            if (camera.isOrtho()) {
                CHECK(glm::length(ray.dir - centre.dir) < 1e-10);
                CHECK(glm::dot(ray.origin - camera.position, camera.forward()) < -65535);
            } else CHECK(ray.origin == camera.position);
        }
    }
    CHECK_THROWS_AS(pixelRay(camera, 1, 0, 0, 0, 0), std::invalid_argument);
}

TEST_CASE("rectFrustum accepts projected interior and handles reversed and zero rectangles") {
    Camera camera;
    camera.position = {-300, 100, 50};
    camera.angles = {12, 43, 0};
    for (const auto kind : {ProjectionKind::Perspective, ProjectionKind::Orthographic}) {
        camera.projection = kind;
        const auto inside = pixelRay(camera, 2, 400, 200, 800, 400).at(66000);
        const auto outside = pixelRay(camera, 2, 700, 200, 800, 400).at(66000);
        const auto planes = rectFrustum(camera, 2, 600, 300, 200, 100, 800, 400);
        CHECK(std::ranges::all_of(planes, [&](Plane p) { return p.signedDistance(inside) <= 1e-6; }));
        CHECK(std::ranges::any_of(planes, [&](Plane p) { return p.signedDistance(outside) > 0; }));
        const auto zero = rectFrustum(camera, 2, 400, 200, 400, 200, 800, 400);
        CHECK(std::ranges::all_of(zero, [&](Plane p) { return std::isfinite(p.dist); }));
        CHECK(std::ranges::all_of(zero, [&](Plane p) { return p.signedDistance(inside) <= 1e-6; }));
    }
}
