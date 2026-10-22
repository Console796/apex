#include "fixtures.hpp"
#include "editor/modal_transform.hpp"

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
GizmoView view() {
    GizmoView v;
    v.cameraPosition = context().pivot - viewDir * 2000.0;
    return v;
}
Ray rayTo(DVec3 point) { return {point - viewDir * 2000.0, viewDir}; }

void type(ModalTransform& modal, std::string_view text) {
    for (const char c : text) {
        const std::string key(1, c);
        CHECK(modal.key(key) == ModalEvent::Updated);
    }
}
ModalTransform started(GizmoMode mode, GizmoContext c = context()) {
    ModalTransform modal;
    REQUIRE(modal.start(mode, c, view(), rayTo(c.pivot), {10, 20}));
    return modal;
}

} // namespace

TEST_CASE("modal G X 32 gives an exact global translation") {
    auto modal = started(GizmoMode::Translate);
    CHECK(modal.active());
    CHECK(modal.screenAnchor() == DVec2{10, 20});
    CHECK(modal.key("X") == ModalEvent::Updated);
    CHECK(modal.constraint() == GizmoHandle::X);
    type(modal, "32");
    CHECK(modal.typedValue() == "32");
    checkPoint(modal.value().delta, {32, 0, 0});
    checkPoint(modal.value().transform.point({1, 2, 3}), {33, 2, 3});
    // Typed values bypass snapping, including fractions and negatives.
    modal.key("Backspace");
    modal.key("Backspace");
    type(modal, "-3.5");
    CHECK(modal.typedValue() == "-3.5");
    checkPoint(modal.value().delta, {-3.5, 0, 0});
    modal.key("-");
    CHECK(modal.typedValue() == "3.5");
    CHECK(modal.key("Enter") == ModalEvent::Commit);
    CHECK_FALSE(modal.active());
    CHECK(modal.key("Enter") == ModalEvent::Ignored);
}

TEST_CASE("modal invalid typed numbers cannot commit") {
    auto modal = started(GizmoMode::Translate);
    modal.key("Y");
    type(modal, "-");
    CHECK_FALSE(modal.valid());
    CHECK(modal.key("Enter") == ModalEvent::Updated);
    CHECK(modal.active());
    modal.key("Backspace");
    type(modal, ".");
    CHECK_FALSE(modal.valid());
    CHECK(modal.key(".") == ModalEvent::Updated); // A second point is ignored.
    CHECK(modal.typedValue() == ".");
    type(modal, "5");
    CHECK(modal.valid());
    checkPoint(modal.value().delta, {0, 0.5, 0});
    CHECK(modal.key("Q") == ModalEvent::Ignored);
    CHECK(modal.key("LeftClick") == ModalEvent::Commit);
}

TEST_CASE("modal escape and right click cancel") {
    for (const char* key : {"Escape", "RightClick"}) {
        auto modal = started(GizmoMode::Translate);
        modal.key("X");
        type(modal, "8");
        CHECK(modal.key(key) == ModalEvent::Cancel);
        CHECK_FALSE(modal.active());
        CHECK(modal.key("Enter") == ModalEvent::Ignored);
    }
}

TEST_CASE("modal rotate and scale with typed values") {
    auto rotate = started(GizmoMode::Rotate);
    rotate.key("Z");
    type(rotate, "45");
    CHECK(rotate.value().angleDeg == doctest::Approx(45));
    const auto c = context();
    checkPoint(rotate.value().transform.point(c.pivot + DVec3{1, 0, 0}),
               c.pivot + DVec3{std::cos(glm::radians(45.0)), std::sin(glm::radians(45.0)), 0});
    rotate.key("Backspace");
    rotate.key("Backspace");
    type(rotate, "-90");
    checkPoint(rotate.value().transform.point(c.pivot + DVec3{1, 0, 0}), c.pivot + DVec3{0, -1, 0});

    auto scale = started(GizmoMode::Scale);
    type(scale, "2");
    checkPoint(scale.value().factors, {2, 2, 2});
    checkPoint(scale.value().transform.point(c.pivot + DVec3{1, 2, 3}), c.pivot + DVec3{2, 4, 6});
    scale.key("X");
    checkPoint(scale.value().factors, {2, 1, 1});
    scale.key("Backspace");
    type(scale, "0");
    CHECK_FALSE(scale.valid()); // A singular scale is rejected and cannot be committed.
    CHECK(scale.key("Enter") == ModalEvent::Updated);
}

TEST_CASE("modal shift axis constrains to the excluded plane and double press goes local") {
    auto c = context();
    c.localFrame = DMat3{Transform::rotate({0, 0, 1}, 30).linear()};
    auto modal = started(GizmoMode::Translate, c);
    modal.key("Z", true);
    CHECK(modal.constraint() == GizmoHandle::XY);
    // Shift+Z moves in the XY plane only, following the mouse.
    modal.update(rayTo(c.pivot + DVec3{20, 14, 0}), {.suspend = true});
    checkPoint(modal.value().delta, {20, 14, 0});
    modal.key("Z", true);
    CHECK(modal.localConstraint());
    CHECK(modal.constraint() == GizmoHandle::XY);

    modal = started(GizmoMode::Translate, c);
    modal.key("X");
    CHECK_FALSE(modal.localConstraint());
    type(modal, "10");
    checkPoint(modal.value().delta, {10, 0, 0});
    modal.key("X");
    CHECK(modal.localConstraint());
    checkPoint(modal.value().delta, DVec3{std::cos(glm::radians(30.0)), std::sin(glm::radians(30.0)), 0} * 10.0);
    CHECK(modal.typedValue() == "10");
    modal.key("X");
    CHECK_FALSE(modal.localConstraint());
    modal.key("Y");
    CHECK(modal.constraint() == GizmoHandle::Y);
    checkPoint(modal.value().delta, {0, 10, 0});
}

TEST_CASE("modal mouse motion snaps and Alt suspends") {
    auto c = context();
    c.snap.mode = SnapMode::Relative;
    auto modal = started(GizmoMode::Translate, c);
    modal.key("X");
    modal.update(rayTo(c.pivot + DVec3{37, 0, 0}));
    CHECK(modal.value().delta.x == doctest::Approx(32));
    CHECK(modal.value().delta.y == doctest::Approx(0));
    modal.update(rayTo(c.pivot + DVec3{37, 0, 0}), {.suspend = true});
    CHECK(modal.value().delta.x == doctest::Approx(37));
}

TEST_CASE("modal mouse rotate and scale follow the pointer") {
    auto c = context();
    c.snap.suspended = true;
    ModalTransform rotate;
    const double radius = 50;
    REQUIRE(rotate.start(GizmoMode::Rotate, c, view(), rayTo(c.pivot + DVec3{radius, 0, 0})));
    rotate.key("Z");
    rotate.update(rayTo(c.pivot + DVec3{0, radius, 0}));
    CHECK(rotate.value().angleDeg == doctest::Approx(90));
    ModalTransform scale;
    REQUIRE(scale.start(GizmoMode::Scale, c, view(), rayTo(c.pivot + DVec3{20, 0, 0})));
    scale.update(rayTo(c.pivot + DVec3{30, 0, 0}));
    CHECK(scale.value().factors.x == doctest::Approx(1.5));
    CHECK(scale.value().factors.y == doctest::Approx(1.5));
}
