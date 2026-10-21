#include "document/vmf.hpp"

#include <doctest/doctest.h>

#include <array>
#include <limits>

using namespace apex;

TEST_CASE("VMF geometry numbers snap and round trip without exponents") {
    const std::array<std::pair<double, std::string>, 12> examples{{
        {63.99999999999999, "64"}, {-63.99999999999999, "-64"},
        {1e-7, "0"}, {-1e-7, "0"}, {-0.0, "0"}, {1e-10, "0"},
        {1.1e-7, "0.00000011"}, {-1.1e-7, "-0.00000011"},
        {12345.678901234567, "12345.678901234567"},
        {-12345.678901234567, "-12345.678901234567"},
        {64.000000002, "64.000000002"}, {1e20, "100000000000000000000"}
    }};
    for (const auto& [number, expected] : examples) {
        CAPTURE(number);
        for (const auto* key : {"point", "origin", "angles"}) {
            auto node = kv1::Node::value(key, "");
            vmf::Value value(&node);
            REQUIRE(value.setGeometry(vmf::Vec3{number, number, number}));
            CHECK(value.text() == expected + " " + expected + " " + expected);
            REQUIRE(value.asVec3());
            if (expected != "0" && expected != "64" && expected != "-64")
                CHECK((*value.asVec3())[0] == number);
        }
        auto node = kv1::Node::value("plane", "");
        vmf::Value plane(&node);
        REQUIRE(plane.set(vmf::PlanePoints{{vmf::Vec3{number, 0, 0},
                                           vmf::Vec3{0, number, 0}, vmf::Vec3{0, 0, number}}}));
        CHECK(plane.text() == "(" + expected + " 0 0) (0 " + expected + " 0) (0 0 " + expected + ")");
    }
    auto node = kv1::Node::value("origin", "unchanged");
    CHECK_FALSE(vmf::Value(&node).setGeometry(vmf::Vec3{std::numeric_limits<double>::infinity(), 0, 0}));
    CHECK(node.decodedValue() == "unchanged");
}

TEST_CASE("VMF non-geometry numbers retain six significant digits") {
    auto node = kv1::Node::value("value", "");
    vmf::Value value(&node);
    REQUIRE(value.set(12345.678901234567));
    CHECK(value.text() == "12345.7");
    REQUIRE(value.set(1e-7));
    CHECK(value.text() == "1e-07");
    REQUIRE(value.set(vmf::Vec3{12345.678901234567, -12345.678901234567, 1e-7}));
    CHECK(value.text() == "12345.7 -12345.7 1e-07");
    const vmf::UvAxis axis{{0.123456789012345, -0.987654321098765, 1.1e-7},
                           12345.678901234567, 0.123456789012345};
    REQUIRE(value.set(axis));
    CHECK(value.text() == "[0.123457 -0.987654 1.1e-07 12345.7] 0.123457");
    REQUIRE(value.setGeometry(axis));
    CHECK(value.text() == "[0.123456789012345 -0.987654321098765 0.00000011 12345.7] 0.123457");
}
