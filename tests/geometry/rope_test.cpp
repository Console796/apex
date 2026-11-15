// Plan 0.8 D14: the rope's resting shape by Source SDK 2013's rope rule (geometry/rope.hpp).
#include "geometry/rope.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace apex;
using namespace apex::geometry;

namespace {

double lowest(const std::vector<DVec3>& points) {
    return std::ranges::min(points, {}, [](DVec3 p) { return p.z; }).z;
}
double length(const std::vector<DVec3>& points) {
    double sum = 0.0;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) sum += glm::length(points[i + 1] - points[i]);
    return sum;
}
RopeSettings rope(int slack, int nodes = 10) {
    RopeSettings s;
    s.slack = slack;
    s.nodes = nodes;
    return s;
}

} // namespace

TEST_CASE("a rope's nodes hold both ends, follow Type's count, and repeat exactly") {
    const DVec3 a{0, 0, 256}, b{512, 0, 256};
    for (const int nodes : {2, 4, 5, 10}) {
        const auto points = ropeNodes(a, b, rope(25, nodes));
        REQUIRE(points.size() == std::size_t(nodes));
        CHECK(points.front() == a);
        CHECK(points.back() == b);
        CHECK(points == ropeNodes(a, b, rope(25, nodes)));
    }
    CHECK(ropeNodes(a, b, rope(25, 40)).size() == 10); // ROPE_MAX_SEGMENTS
    CHECK(ropeNodes(a, b, rope(25, 1)).size() == 2);
}

TEST_CASE("a level rope hangs nearly symmetrically and lower with more slack") {
    const DVec3 a{0, 0, 0}, b{512, 0, 0};
    double previous = 0.0;
    for (const int slack : {0, 25, 100, 400}) {
        INFO("slack ", slack);
        const auto points = ropeNodes(a, b, rope(slack));
        // The engine's constraint passes sweep from the start, so a rope held tighter than its
        // span leans a little toward the end. The lean is the engine's too.
        for (std::size_t i = 0; i < points.size(); ++i) {
            CHECK(std::abs(points[i].z - points[points.size() - 1 - i].z) < 0.5);
            CHECK(points[i].y == doctest::Approx(0.0));
        }
        const double low = lowest(points);
        CHECK(low < previous); // Even Slack 0 sags: the -100 makes the rest length shorter than the span.
        previous = low;
    }
}

TEST_CASE("a slack rope settles at its rest length, a tight one stretches between its ends") {
    const DVec3 a{0, 0, 0}, b{300, 0, 0};
    // Rest length (300 + 400 - 100) / 9 per link: the links end at it, give or take the pull of gravity.
    const auto slack = ropeNodes(a, b, rope(400));
    CHECK(length(slack) == doctest::Approx(600.0).epsilon(0.01));
    // (300 + 0 - 100) is shorter than the span, so every link is stretched past its rest length.
    const auto tight = ropeNodes(a, b, rope(0));
    CHECK(length(tight) > 300.0);
    CHECK(lowest(tight) > -60.0);
    // A rest length below zero is zero: the links pull as hard as they can.
    CHECK(lowest(ropeNodes(a, b, rope(-500))) > lowest(tight));
}

TEST_CASE("a dangling rope hangs straight down from its start") {
    RopeSettings s = rope(300);
    s.dangling = true;
    const auto points = ropeNodes({0, 0, 0}, {200, 0, 0}, s);
    CHECK(points.front() == DVec3{0, 0, 0});
    for (const auto& p : points) CHECK(std::abs(p.x) < 1.0);
    // int(200) + 300 - 100 over the nine links, stretched about 1% by gravity: three passes a
    // step do not quite hold a hanging rope's weight.
    CHECK(points.back().z < -400.0);
    CHECK(points.back().z > -410.0);
}

TEST_CASE("the drawn curve passes through each node with Subdiv Catmull-Rom points between") {
    const auto nodes = ropeNodes({0, 0, 0}, {512, 0, 0}, rope(100));
    for (const int subdiv : {0, 2, 7}) {
        const auto curve = ropeCurve(nodes, subdiv);
        REQUIRE(curve.size() == nodes.size() + (nodes.size() - 1) * subdiv);
        for (std::size_t i = 0; i < nodes.size(); ++i) CHECK(curve[i * (subdiv + 1)] == nodes[i]);
    }
    CHECK(ropeCurve(nodes, 255).size() == ropeCurve(nodes, 2).size()); // rope_subdiv's default
    CHECK(ropeCurve(nodes, 8).size() == ropeCurve(nodes, 7).size());   // MAX_ROPE_SUBDIVS - 1
    // Between two nodes of a straight run the spline stays on it.
    const std::vector<DVec3> line{{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}};
    for (const auto& p : ropeCurve(line, 3)) CHECK(glm::length(DVec3{p.x, 0, 0} - p) < 1e-12);
    CHECK(ropeShape({0, 0, 0}, {512, 0, 0}, rope(100)) == ropeCurve(nodes, 2));
}
