#include "viewport/controllers.hpp"
#include "viewport/view_context.hpp"
#include "viewport/viewport.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::viewport;

namespace {
void checkVec(DVec3 a, DVec3 b, double eps = 1e-9) {
    CHECK(a.x == doctest::Approx(b.x).epsilon(eps).scale(1.0));
    CHECK(a.y == doctest::Approx(b.y).epsilon(eps).scale(1.0));
    CHECK(a.z == doctest::Approx(b.z).epsilon(eps).scale(1.0));
}
} // namespace

TEST_CASE("axis angles look along their axis with Hammer screen orientation") {
    for (auto axis : {ViewAxis::PosX, ViewAxis::NegX, ViewAxis::PosY, ViewAxis::NegY,
                      ViewAxis::PosZ, ViewAxis::NegZ}) {
        CAPTURE(nameOf(axis));
        Camera c;
        setAxis(c, axis);
        checkVec(c.forward(), *axisDirection(axis));
    }
    Camera top;
    setAxis(top, ViewAxis::NegZ);
    checkVec(top.basis().right, {1, 0, 0});
    checkVec(top.basis().up, {0, 1, 0});
    Camera front;
    setAxis(front, ViewAxis::PosY);
    checkVec(front.basis().right, {1, 0, 0});
    checkVec(front.basis().up, {0, 0, 1});
    Camera side;
    setAxis(side, ViewAxis::NegX);
    checkVec(side.basis().right, {0, 1, 0});
    checkVec(side.basis().up, {0, 0, 1});
}

TEST_CASE("frame bounds centres the view without turning it") {
    const AABB box{{100, 200, 0}, {164, 264, 64}};
    const DVec3 centre{132, 232, 32};
    Camera perspective;
    perspective.angles = {30, 45, 0};
    perspective.position = {-5000, 0, 900};
    frameBounds(perspective, box, 16.0 / 9.0);
    checkVec(perspective.angles, {30, 45, 0});
    checkVec(perspective.pivot(), centre, 1e-6);
    // The bounding sphere fits the vertical field of view, the narrower one here.
    const double radius = glm::length(box.max - box.min) * 0.5 * 1.25;
    CHECK(perspective.orbitDistance == doctest::Approx(radius / std::sin(glm::radians(37.5))));

    Camera top;
    setAxis(top, ViewAxis::NegZ);
    top.projection = ProjectionKind::Orthographic;
    top.position = {0, 0, 4096};
    frameBounds(top, box, 0.5);
    checkVec(top.position, {132, 232, 4096}); // Depth along the view direction is kept.
    CHECK(top.orthoHeight == doctest::Approx(4.0 * radius)); // Narrow pane: width limits.

    Camera untouched = top;
    frameBounds(untouched, AABB{}, 1.0);
    checkVec(untouched.position, top.position);
    CHECK(untouched.orthoHeight == top.orthoHeight);

    Camera point;
    frameBounds(point, AABB{{8, 8, 8}, {8, 8, 8}}, 1.0);
    CHECK(point.orbitDistance > 16.0); // A point entity still gets a usable view.
    checkVec(point.pivot(), {8, 8, 8}, 1e-6);
}

TEST_CASE("presets") {
    for (auto p : {Preset::Top, Preset::Front, Preset::Side}) {
        const auto vp = makePreset(p, {10, 20, 30});
        CHECK(vp.camera.isOrtho());
        CHECK(vp.camera.axisLocked());
        CHECK(vp.mode == DisplayMode::Wireframe);
        CHECK(vp.overlays.grid);
        checkVec(vp.camera.position, {10, 20, 30});
    }
    const auto cam = makePreset(Preset::Camera, {0, 0, 0});
    CHECK_FALSE(cam.camera.isOrtho());
    CHECK_FALSE(cam.camera.axisLocked());
    CHECK(cam.mode == DisplayMode::Textured);
    checkVec(cam.camera.pivot(), {0, 0, 0}, 1e-6);

    auto vp = makePreset(Preset::Camera, {5, 6, 7});
    applyPreset(vp, Preset::Top);
    CHECK(vp.camera.isOrtho());
    checkVec(vp.camera.position, {5, 6, 7}, 1e-6);
    applyPreset(vp, Preset::Camera);
    CHECK_FALSE(vp.camera.isOrtho());
    checkVec(vp.camera.pivot(), {5, 6, 7}, 1e-6);
}

TEST_CASE("viewport JSON round trip and validation") {
    auto vp = makePreset(Preset::Front, {1, 2, 3}, 777);
    vp.overlays.normals = true;
    const auto text = json::write(toJson(vp));
    const auto parsed = json::parse(text);
    REQUIRE(parsed);
    const auto back = viewportFromJson(*parsed);
    REQUIRE(back);
    CHECK(back->camera.axis == ViewAxis::PosY);
    CHECK(back->camera.orthoHeight == 777);
    CHECK(back->mode == DisplayMode::Wireframe);
    CHECK(back->overlays.normals);
    CHECK(json::write(toJson(*back)) == text);

    CHECK_FALSE(viewportFromJson(*json::parse(R"({"mode":"nope"})")));
    CHECK_FALSE(viewportFromJson(*json::parse(R"({"camera":{"fov":0}})")));
    CHECK_FALSE(viewportFromJson(*json::parse(R"([])")));
}

TEST_CASE("fly controller: RMB look and WASD") {
    Camera c;
    NavSettings s;
    NavInput in;
    in.cursorDelta = {40, 0};
    CHECK_FALSE(navigate(c, in, s)); // RMB not held.
    in.right = true;
    CHECK(navigate(c, in, s));
    CHECK(c.angles.y == doctest::Approx(360.0 - 40 * s.lookDegreesPerPixel));
    in.cursorDelta = {0, 100000};
    navigate(c, in, s);
    CHECK(c.angles.x == s.maxPitch);

    Camera d;
    NavInput move;
    move.right = true;
    move.moveForward = true;
    move.dt = 0.5;
    navigate(d, move, s);
    checkVec(d.position, {s.flySpeed * 0.5, 0, 0});
    move.shift = true;
    Camera e;
    navigate(e, move, s);
    CHECK(e.position.x == doctest::Approx(s.flySpeed * s.fastMultiplier * 0.5));
    CHECK(e.controller == ControllerKind::Fly);
}

TEST_CASE("orbit keeps the pivot; shift pans; scroll dollies") {
    Camera c;
    c.position = {-300, 0, 0};
    c.orbitDistance = 300;
    const DVec3 pivot = c.pivot();
    checkVec(pivot, {0, 0, 0});
    NavInput in;
    in.middle = true;
    in.cursorDelta = {120, -60};
    CHECK(navigate(c, in));
    checkVec(c.pivot(), pivot, 1e-9);
    CHECK(c.controller == ControllerKind::Orbit);

    const DVec3 before = c.position;
    in.shift = true;
    in.cursorDelta = {50, 0};
    CHECK(navigate(c, in));
    const DVec3 moved = c.position - before;
    CHECK(glm::length(moved) > 0.0);
    CHECK(glm::dot(moved, c.forward()) == doctest::Approx(0.0).epsilon(1e-9));

    NavInput scroll;
    scroll.scroll = 2;
    const DVec3 p2 = c.pivot();
    const double d0 = c.orbitDistance;
    CHECK(navigate(c, scroll));
    CHECK(c.orbitDistance < d0);
    checkVec(c.pivot(), p2, 1e-9);
}

TEST_CASE("axis-locked perspective never rotates") {
    Camera c;
    setAxis(c, ViewAxis::PosY);
    const auto angles = c.angles;
    NavInput in;
    in.middle = true;
    in.right = true;
    in.cursorDelta = {50, 50};
    navigate(c, in);
    checkVec(c.angles, angles);
}

TEST_CASE("ortho pan and zoom about the cursor") {
    auto vp = makePreset(Preset::Top, {0, 0, 0}, 1000);
    auto& c = vp.camera;
    NavInput in;
    in.viewportSize = {1000, 500};
    in.middle = true;
    in.cursorDelta = {100, 0}; // 2 units per pixel.
    CHECK(navigate(c, in));
    checkVec(c.position, {-200, 0, 0}); // Top: +X is screen right, dragging right moves the view left.
    in.cursorDelta = {0, 50};
    navigate(c, in);
    checkVec(c.position, {-200, 100, 0}); // Dragging down moves the view up (+Y).
    CHECK(c.controller == ControllerKind::Pan2D);

    // Zoom keeps the world point under the cursor fixed.
    NavInput z;
    z.viewportSize = {1000, 500};
    z.cursor = {800, 100};
    z.scroll = 3;
    const auto before = cursorRay(c, z.cursor, z.viewportSize).origin;
    CHECK(navigate(c, z));
    CHECK(c.orthoHeight < 1000);
    checkVec(cursorRay(c, z.cursor, z.viewportSize).origin, before, 1e-6);
}

TEST_CASE("view context rays and work planes") {
    Viewport persp = makePreset(Preset::Camera);
    persp.camera.position = {0, 0, 100};
    persp.camera.angles = {0, 0, 0};
    const auto ctx = makeViewContext(persp, {500, 500}, {1000, 1000});
    CHECK(ctx.projection == ProjectionKind::Perspective);
    checkVec(ctx.cursorRay.dir, {1, 0, 0});
    CHECK_FALSE(ctx.workPlane.intersect(ctx.cursorRay)); // Parallel to the ground.

    persp.camera.angles = {45, 0, 0};
    const auto down = makeViewContext(persp, {500, 500}, {1000, 1000});
    const auto t = down.workPlane.intersect(down.cursorRay);
    REQUIRE(t);
    checkVec(down.cursorRay.at(*t), {100, 0, 0}, 1e-9);

    Viewport top = makePreset(Preset::Top, {0, 0, 0}, 1000);
    AABB sel;
    sel.expand({0, 0, 0});
    sel.expand({64, 64, 128});
    const auto tctx = makeViewContext(top, {750, 250}, {1000, 1000}, {16, true}, sel);
    CHECK(tctx.axisLocked());
    CHECK(tctx.projection == ProjectionKind::Orthographic);
    checkVec(tctx.viewDirection, {0, 0, -1});
    checkVec(tctx.workPlane.normal, {0, 0, 1});
    CHECK(tctx.workPlane.origin.z == doctest::Approx(64));
    checkVec(tctx.cursorRay.origin, {250, 250, 0}); // Right 250, up 250 of an 1000-high view.
    const auto hit = tctx.workPlane.intersect(tctx.cursorRay, true);
    REQUIRE(hit);
    checkVec(tctx.cursorRay.at(*hit), {250, 250, 64});
    CHECK(tctx.grid.snapValue(250) == 256);
    CHECK(GridSettings{16, false}.snapValue(250) == 250);
}

TEST_CASE("camera matrices are finite") {
    const auto persp = makePreset(Preset::Camera);
    const auto p = persp.camera.projectionMatrix(1.5);
    CHECK(p[1][1] > 0.0f);
    const auto o = makePreset(Preset::Top).camera.projectionMatrix(2.0);
    CHECK(o[0][0] == doctest::Approx(1.0 / (512.0 * 2.0)));
}
