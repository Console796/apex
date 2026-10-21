#include "viewport/layout.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::viewport;

TEST_CASE("layout split, close, maximize") {
    ViewportLayout l;
    CHECK(l.paneCount() == 1);
    const NodeId a = l.paneIds().front();
    CHECK_FALSE(l.close(a)); // Last pane.

    const auto b = l.split(a, Orientation::Horizontal, 0.25);
    REQUIRE(b);
    CHECK(l.paneCount() == 2);
    CHECK(l.paneIds() == std::vector<NodeId>{a, *b});
    auto rects = l.layout({0, 0, 100, 100});
    REQUIRE(rects.size() == 2);
    CHECK(rects[0].rect == Rect{0, 0, 25, 100});
    CHECK(rects[1].rect == Rect{25, 0, 75, 100});

    const auto c = l.split(*b, Orientation::Vertical);
    REQUIRE(c);
    rects = l.layout({0, 0, 100, 100});
    REQUIRE(rects.size() == 3);
    CHECK(rects[1].rect == Rect{25, 0, 75, 50});
    CHECK(rects[2].rect == Rect{25, 50, 75, 50});
    CHECK(l.splits().size() == 2);

    REQUIRE(l.maximize(*c));
    CHECK(l.maximized() == *c);
    CHECK(l.layout({0, 0, 10, 10}).size() == 1);
    l.restore();
    CHECK(l.layout().size() == 3);

    CHECK(l.maximize(*c));
    REQUIRE(l.close(*c)); // Closing the maximized pane restores.
    CHECK_FALSE(l.maximized());
    CHECK(l.paneCount() == 2);
    rects = l.layout({0, 0, 100, 100});
    CHECK(rects[1].rect == Rect{25, 0, 75, 100}); // Sibling took the parent's place.

    CHECK_FALSE(l.close(9999));
    REQUIRE(l.setActive(*b));
    REQUIRE(l.close(*b));
    CHECK(l.active() == a);
    CHECK(l.paneCount() == 1);
}

TEST_CASE("split copies the viewport and ratio is clamped") {
    ViewportLayout l(makePreset(Preset::Top, {1, 2, 3}, 500));
    const NodeId a = l.paneIds().front();
    const NodeId b = *l.split(a, Orientation::Vertical, 5.0);
    CHECK(l.viewport(b)->camera.orthoHeight == 500);
    CHECK(l.splits().front().ratio == 0.95);
    REQUIRE(l.setRatio(l.splits().front().split, 0.0));
    CHECK(l.splits().front().ratio == 0.05);
    CHECK_FALSE(l.setRatio(a, 0.5));
}

TEST_CASE("quad layout and JSON round trip") {
    auto l = ViewportLayout::quad();
    CHECK(l.paneCount() == 4);
    const auto ids = l.paneIds();
    CHECK_FALSE(l.viewport(ids[0])->camera.isOrtho());
    for (int i = 1; i < 4; ++i) CHECK(l.viewport(ids[static_cast<std::size_t>(i)])->camera.isOrtho());
    CHECK(l.viewport(ids[1])->camera.axis == ViewAxis::NegZ);
    CHECK(l.viewport(ids[2])->camera.axis == ViewAxis::PosY);
    CHECK(l.viewport(ids[3])->camera.axis == ViewAxis::NegX);
    REQUIRE(l.maximize(ids[2]));

    const auto text = json::write(l.toJson());
    const auto parsed = json::parse(text);
    REQUIRE(parsed);
    auto back = ViewportLayout::fromJson(*parsed);
    REQUIRE(back);
    CHECK(back->paneIds() == ids);
    CHECK(back->maximized() == ids[2]);
    CHECK(back->active() == l.active());
    CHECK(json::write(back->toJson()) == text);
    // Ids keep growing past the loaded ones.
    const auto n = back->split(ids[0], Orientation::Horizontal);
    REQUIRE(n);
    for (auto id : ids) CHECK(*n != id);

    ViewportLayout copy = *back;
    CHECK(copy.paneCount() == 5);
    CHECK(json::write(copy.toJson()) == json::write(back->toJson()));
}

TEST_CASE("layout JSON rejects malformed input") {
    const auto bad = [](const char* text) {
        const auto parsed = json::parse(text);
        REQUIRE(parsed);
        return !ViewportLayout::fromJson(*parsed).has_value();
    };
    CHECK(bad("[]"));
    CHECK(bad("{}"));
    CHECK(bad(R"({"root":{"id":1,"type":"mystery"}})"));
    CHECK(bad(R"({"root":{"id":1,"type":"split","orientation":"horizontal","ratio":0.5,
        "first":{"id":2,"type":"pane","viewport":{}},"second":{"id":2,"type":"pane","viewport":{}}}})"));
    CHECK(bad(R"({"root":{"id":1,"type":"split","orientation":"horizontal","ratio":2,
        "first":{"id":2,"type":"pane","viewport":{}},"second":{"id":3,"type":"pane","viewport":{}}}})"));
    CHECK(bad(R"({"root":{"id":1,"type":"split","orientation":"diagonal","ratio":0.5,
        "first":{"id":2,"type":"pane","viewport":{}},"second":{"id":3,"type":"pane","viewport":{}}}})"));
    CHECK_FALSE(bad(R"({"root":{"id":4,"type":"pane","viewport":{}}})"));
}
