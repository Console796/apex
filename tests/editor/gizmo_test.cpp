#include "fixtures.hpp"
#include "editor/gizmo.hpp"

#include <array>

using namespace editor_test;

namespace {

const DVec3 viewDir = glm::normalize(DVec3{-0.6, -0.7, -0.5});

GizmoContext context() {
    GizmoContext c;
    c.bounds = AABB{{5, 5, 5}, {69, 69, 69}};
    c.pivot = c.bounds.center();
    c.snap.gridSize = 16;
    return c;
}
GizmoView view(double ppu = 1.0) {
    GizmoView v;
    v.pixelsPerWorldUnit = ppu;
    v.cameraPosition = context().pivot - viewDir * 2000.0;
    return v;
}
// Parallel (orthographic-style) ray through a world point, offset in the image plane.
Ray rayTo(DVec3 point, DVec3 offset = DVec3{0.0}) {
    return {point + offset - viewDir * 2000.0, viewDir};
}
DVec3 perpendicular(DVec3 a) { return glm::normalize(glm::cross(viewDir, a)); }

GizmoPrimitive primitive(GizmoMode mode, GizmoHandle handle, double ppu = 1.0) {
    for (const auto& p : gizmoDescription(mode, context(), view(ppu)))
        if (p.handle == handle) return p;
    FAIL("missing handle");
    return {};
}
// A point comfortably on the primitive, away from the other handles.
DVec3 anchor(const GizmoPrimitive& p) {
    if (p.shape == GizmoShape::Segment) return p.points[0] * 0.4 + p.points[1] * 0.6;
    if (p.shape == GizmoShape::Quad) return (p.points[0] + p.points[2]) * 0.5;
    return p.centre + glm::normalize(glm::cross(p.normal, DVec3{1, 2, 3})) * p.radius;
}
const std::array<std::pair<GizmoMode, std::vector<GizmoHandle>>, 3> kHandles{{
    {GizmoMode::Translate, {GizmoHandle::X, GizmoHandle::Y, GizmoHandle::Z, GizmoHandle::XY,
                            GizmoHandle::YZ, GizmoHandle::ZX, GizmoHandle::Screen}},
    {GizmoMode::Rotate, {GizmoHandle::X, GizmoHandle::Y, GizmoHandle::Z, GizmoHandle::Screen}},
    {GizmoMode::Scale, {GizmoHandle::X, GizmoHandle::Y, GizmoHandle::Z, GizmoHandle::Uniform}}}};

} // namespace

TEST_CASE("gizmo handle sets per mode match the design") {
    for (const auto& [mode, handles] : kHandles) {
        std::vector<GizmoHandle> found;
        for (const auto& p : gizmoDescription(mode, context(), view())) found.push_back(p.handle);
        CHECK(found == handles);
    }
    for (const auto& p : gizmoDescription(GizmoMode::Rotate, context(), view()))
        CHECK(p.shape == GizmoShape::Ring);
    CHECK_THROWS_AS(gizmoDescription(GizmoMode::Translate, context(), view(0.0)), std::invalid_argument);
}

TEST_CASE("gizmo description is constant size on screen") {
    for (const auto& [mode, handles] : kHandles) {
        const auto near = gizmoDescription(mode, context(), view(1.0));
        const auto far = gizmoDescription(mode, context(), view(4.0));
        REQUIRE(near.size() == far.size());
        for (std::size_t i = 0; i < near.size(); ++i) {
            CHECK(near[i].handle == far[i].handle);
            CHECK(near[i].radius == doctest::Approx(far[i].radius * 4.0));
            if (near[i].shape == GizmoShape::Ring) continue;
            for (int k = 0; k < (near[i].shape == GizmoShape::Quad ? 4 : 2); ++k)
                checkPoint(near[i].points[k] - context().pivot, (far[i].points[k] - context().pivot) * 4.0);
        }
    }
}

TEST_CASE("gizmo hit test finds every handle from a known ray") {
    for (const auto& [mode, handles] : kHandles) {
        for (const auto handle : handles) {
            CAPTURE(static_cast<int>(mode));
            CAPTURE(static_cast<int>(handle));
            for (const double ppu : {0.5, 1.0, 3.0}) {
                const auto p = primitive(mode, handle, ppu);
                const auto hit = hitTestGizmo(mode, context(), view(ppu), rayTo(anchor(p)));
                REQUIRE(hit);
                CHECK(hit->handle == handle);
                CHECK(hit->distancePixels < 0.01);
            }
        }
    }
    // A ray well away from the gizmo hits nothing, as do invalid rays.
    CHECK_FALSE(hitTestGizmo(GizmoMode::Translate, context(), view(), rayTo({500, 500, 500})));
    CHECK_FALSE(hitTestGizmo(GizmoMode::Translate, context(), view(), Ray{{0, 0, 0}, {0, 0, 0}}));
    CHECK_FALSE(hitTestGizmo(GizmoMode::Translate, context(), view(), rayTo({0, 0, 0}), -1));
}

TEST_CASE("gizmo hit tolerance is measured in pixels and scales with zoom") {
    const auto p = primitive(GizmoMode::Translate, GizmoHandle::Y);
    const auto point = anchor(p);
    const auto side = perpendicular(p.points[1] - p.points[0]);
    for (const double ppu : {0.5, 1.0, 3.0}) {
        CAPTURE(ppu);
        const auto q = anchor(primitive(GizmoMode::Translate, GizmoHandle::Y, ppu));
        const auto inside = hitTestGizmo(GizmoMode::Translate, context(), view(ppu), rayTo(q, side * (5.5 / ppu)), 6.0);
        REQUIRE(inside);
        CHECK(inside->handle == GizmoHandle::Y);
        CHECK(inside->distancePixels == doctest::Approx(5.5));
        const auto outside = hitTestGizmo(GizmoMode::Translate, context(), view(ppu), rayTo(q, side * (6.5 / ppu)), 6.0);
        CHECK_FALSE(outside);
        CHECK(hitTestGizmo(GizmoMode::Translate, context(), view(ppu), rayTo(q, side * (6.5 / ppu)), 7.0));
    }
    (void)point;
}

TEST_CASE("gizmo nearest handle along the ray wins") {
    // The X axis and the XY plane overlap on screen when viewed down Z; the nearer one wins.
    auto v = view();
    v.cameraPosition = context().pivot + DVec3{0, 0, 2000};
    const Ray down{context().pivot + DVec3{40, 30, 2000}, {0, 0, -1}};
    const auto hit = hitTestGizmo(GizmoMode::Translate, context(), v, down);
    REQUIRE(hit);
    CHECK(hit->handle == GizmoHandle::XY);
    // Raising the plane's near edge region: a ray along the X axis itself still picks X.
    const Ray along{context().pivot + DVec3{60, 0, 2000}, {0, 0, -1}};
    const auto axis = hitTestGizmo(GizmoMode::Translate, context(), v, along);
    REQUIRE(axis);
    CHECK(axis->handle == GizmoHandle::X);
}

TEST_CASE("gizmo translate drag along axes and planes in both snap modes") {
    for (const auto handle : {GizmoHandle::X, GizmoHandle::Y, GizmoHandle::Z, GizmoHandle::XY,
                              GizmoHandle::YZ, GizmoHandle::ZX}) {
        for (const auto snapMode : {SnapMode::Absolute, SnapMode::Relative}) {
            CAPTURE(static_cast<int>(handle));
            auto c = context();
            c.snap.mode = snapMode;
            const auto p = primitive(GizmoMode::Translate, handle);
            const auto start = anchor(p);
            const DVec3 axes = handle == GizmoHandle::X ? DVec3{1, 0, 0}
                : handle == GizmoHandle::Y ? DVec3{0, 1, 0} : handle == GizmoHandle::Z ? DVec3{0, 0, 1}
                : handle == GizmoHandle::XY ? DVec3{1, 1, 0} : handle == GizmoHandle::YZ ? DVec3{0, 1, 1}
                : DVec3{1, 0, 1};
            const DVec3 raw = axes * 37.0;
            GizmoDrag drag;
            REQUIRE(drag.begin(GizmoMode::Translate, handle, c, view(), rayTo(start)));
            const auto& result = drag.update(rayTo(start + raw));
            // The constrained delta never leaves the handle's axes and lands on the grid.
            for (int i = 0; i < 3; ++i) {
                if (axes[i] == 0) CHECK(result.delta[i] == doctest::Approx(0));
                else {
                    const double grid = snapMode == SnapMode::Absolute
                        ? c.bounds.min[i] + result.delta[i] : result.delta[i];
                    CHECK(grid / 16.0 == doctest::Approx(std::round(grid / 16.0)));
                    CHECK(std::abs(result.delta[i] - 37.0) <= 16.0);
                }
            }
            checkPoint(result.transform.point({1, 2, 3}), DVec3{1, 2, 3} + result.delta);
            // Alt suspends snapping entirely.
            const auto suspended = drag.update(rayTo(start + raw), {.suspend = true});
            checkPoint(suspended.delta, raw);
            CHECK(drag.value().text.starts_with("Move: "));
        }
    }
}

TEST_CASE("gizmo translate screen handle moves in the work plane") {
    auto c = context();
    c.snap.mode = SnapMode::Relative;
    GizmoDrag drag;
    const auto start = c.pivot;
    REQUIRE(drag.begin(GizmoMode::Translate, GizmoHandle::Screen, c, view(), rayTo(start)));
    const auto& result = drag.update(rayTo(start + DVec3{1, 0, 0} * 21.0));
    CHECK(result.delta.x != 0);
    CHECK(std::abs(result.delta.x - 21.0) <= 16.0);
    // Dragging a ray that misses its plane retains the previous value.
    const auto before = result.delta;
    CHECK(drag.update(Ray{start, {0, 0, 0}}).delta == before);
}

TEST_CASE("gizmo rotate drag snaps by angle and honours fine and suspend") {
    for (const auto handle : {GizmoHandle::X, GizmoHandle::Y, GizmoHandle::Z}) {
        CAPTURE(static_cast<int>(handle));
        auto c = context();
        c.snap.mode = SnapMode::Relative;
        const int axis = static_cast<int>(handle);
        const DVec3 normal = DVec3{axis == 0, axis == 1, axis == 2};
        const DVec3 u = glm::normalize(glm::cross(normal, DVec3{1, 2, 3}));
        const DVec3 w = glm::cross(normal, u);
        const double radius = primitive(GizmoMode::Rotate, handle).radius;
        const auto at = [&](double degrees) {
            const double r = glm::radians(degrees);
            return c.pivot + (u * std::cos(r) + w * std::sin(r)) * radius;
        };
        GizmoDrag drag;
        REQUIRE(drag.begin(GizmoMode::Rotate, handle, c, view(), rayTo(at(0))));
        CHECK(drag.update(rayTo(at(50))).angleDeg == doctest::Approx(45));
        CHECK(drag.update(rayTo(at(50)), {.fine = true}).angleDeg == doctest::Approx(50));
        CHECK(drag.update(rayTo(at(50.4)), {.suspend = true}).angleDeg == doctest::Approx(50.4));
        const auto& rotated = drag.update(rayTo(at(50)));
        checkPoint(rotated.transform.point(c.pivot + u), c.pivot + u * std::cos(glm::radians(45.0)) +
                                                            w * std::sin(glm::radians(45.0)));
        checkPoint(rotated.transform.point(c.pivot), c.pivot);
        CHECK(rotated.text.starts_with("Rotate: "));
    }
    // Absolute mode snaps the resulting angle in the frame, so a 7 degree start rounds to 15.
    auto c = context();
    c.rotationStartDeg = {0, 0, 7};
    const DVec3 u{1, 0, 0}, w{0, 1, 0};
    const double radius = primitive(GizmoMode::Rotate, GizmoHandle::Z).radius;
    GizmoDrag drag;
    REQUIRE(drag.begin(GizmoMode::Rotate, GizmoHandle::Z, c, view(), rayTo(c.pivot + u * radius)));
    const double r = glm::radians(10.0);
    CHECK(drag.update(rayTo(c.pivot + (u * std::cos(r) + w * std::sin(r)) * radius)).angleDeg ==
          doctest::Approx(8));
}

TEST_CASE("gizmo rotate drag accumulates past a half turn") {
    auto c = context();
    c.snap.suspended = true;
    const double radius = primitive(GizmoMode::Rotate, GizmoHandle::Z).radius;
    const auto at = [&](double degrees) {
        const double r = glm::radians(degrees);
        return c.pivot + DVec3{std::cos(r), std::sin(r), 0} * radius;
    };
    GizmoDrag drag;
    REQUIRE(drag.begin(GizmoMode::Rotate, GizmoHandle::Z, c, view(), rayTo(at(0))));
    for (double a = 30; a <= 400; a += 30) drag.update(rayTo(at(a)));
    CHECK(drag.update(rayTo(at(400))).angleDeg == doctest::Approx(400));
}

TEST_CASE("gizmo scale drag per axis and uniform in both snap modes") {
    for (const auto handle : {GizmoHandle::X, GizmoHandle::Y, GizmoHandle::Z}) {
        for (const auto snapMode : {SnapMode::Absolute, SnapMode::Relative}) {
            CAPTURE(static_cast<int>(handle));
            auto c = context();
            c.snap.mode = snapMode;
            const int axis = static_cast<int>(handle);
            const DVec3 direction{axis == 0, axis == 1, axis == 2};
            const auto start = c.pivot + direction * 40.0;
            GizmoDrag drag;
            REQUIRE(drag.begin(GizmoMode::Scale, handle, c, view(), rayTo(start)));
            const auto& result = drag.update(rayTo(c.pivot + direction * 62.0));
            for (int i = 0; i < 3; ++i) {
                if (i != axis) CHECK(result.factors[i] == doctest::Approx(1));
                else {
                    CHECK(result.factors[i] > 1.2);
                    // Absolute: the moving (far) endpoint is on the grid. Relative: the size change is.
                    const double measure = snapMode == SnapMode::Absolute
                        ? c.pivot[i] + (c.bounds.max[i] - c.pivot[i]) * result.factors[i]
                        : (c.bounds.max[i] - c.bounds.min[i]) * (result.factors[i] - 1.0);
                    CHECK(measure / 16.0 == doctest::Approx(std::round(measure / 16.0)));
                }
            }
            const auto exact = drag.update(rayTo(c.pivot + direction * 62.0), {.suspend = true});
            CHECK(exact.factors[axis] == doctest::Approx(62.0 / 40.0));
            checkPoint(exact.transform.point(c.pivot + direction * 10.0),
                       c.pivot + direction * 10.0 * (62.0 / 40.0));
        }
    }
    auto c = context();
    c.snap.suspended = true;
    GizmoDrag drag;
    const auto p = primitive(GizmoMode::Scale, GizmoHandle::Uniform);
    REQUIRE(drag.begin(GizmoMode::Scale, GizmoHandle::Uniform, c, view(), rayTo(anchor(p))));
    // Pressing at the exact centre still starts a drag.
    CHECK(drag.begin(GizmoMode::Scale, GizmoHandle::Uniform, c, view(), rayTo(c.pivot)));
    const auto& uniform = drag.update(rayTo(c.pivot + perpendicular(DVec3{0, 0, 1}) * 20.0));
    CHECK(uniform.factors.x == doctest::Approx(uniform.factors.y));
    CHECK(uniform.factors.y == doctest::Approx(uniform.factors.z));
    CHECK(uniform.text.starts_with("Scale: "));
}

TEST_CASE("gizmo drag rejects absent handles and cancels") {
    GizmoDrag drag;
    CHECK_FALSE(drag.begin(GizmoMode::Scale, GizmoHandle::XY, context(), view(), rayTo(context().pivot)));
    CHECK_FALSE(drag.begin(GizmoMode::Rotate, GizmoHandle::XY, context(), view(), rayTo(context().pivot)));
    CHECK_FALSE(drag.active());
    const auto p = primitive(GizmoMode::Translate, GizmoHandle::X);
    REQUIRE(drag.begin(GizmoMode::Translate, GizmoHandle::X, context(), view(), rayTo(anchor(p))));
    CHECK(drag.active());
    drag.cancel();
    CHECK_FALSE(drag.active());
    const auto value = drag.value().text;
    CHECK(drag.update(rayTo(anchor(p) + DVec3{50, 0, 0})).text == value);
    // A ray parallel to the axis constraint cannot start the drag.
    CHECK_FALSE(drag.begin(GizmoMode::Translate, GizmoHandle::X, context(), view(), Ray{anchor(p), {1, 0, 0}}));
}
