#include "core/strings.hpp"

#include <doctest/doctest.h>

#include <array>
#include <ostream>
#include <string>
#include <string_view>

TEST_CASE("ASCII case helpers agree and preserve non-ASCII bytes") {
    CHECK(apex::ciEquals("Material/BRICK", "material/brick"));
    CHECK(apex::ciEquals("", ""));
    CHECK_FALSE(apex::ciEquals("brick", "bricks"));
    CHECK_FALSE(apex::ciEquals("a", "b"));
    CHECK(apex::ciLess("abc", "ABCD"));
    CHECK(apex::ciLess("Ab", "ac"));
    CHECK_FALSE(apex::ciLess("AbC", "aBc"));
    CHECK_FALSE(apex::ciLess("b", "A"));
    CHECK(apex::ciLess("", "a"));
    CHECK(apex::toLowerAscii("AZaz09_\\/") == "azaz09_\\/");
    const std::string bytes{"A\xC0\xE0\0Z", 5};
    const std::string lowered{"a\xC0\xE0\0z", 5};
    CHECK(apex::toLowerAscii(bytes) == lowered);
    CHECK(apex::ciEquals(bytes, lowered));
    CHECK_FALSE(apex::ciEquals("\xC0", "\xE0"));
    CHECK(apex::ciLess("z", "\xC0"));
    constexpr std::array<std::string_view, 5> samples{"", "Foo", "FOO", "foo", "Foo/bar"};
    for (auto a : samples) {
        for (auto b : samples) {
            if (apex::ciEquals(a, b)) {
                CHECK(apex::CiHash{}(a) == apex::CiHash{}(b));
            }
        }
    }
    CHECK(apex::CiHash{}(bytes) == apex::CiHash{}(lowered));
}

TEST_CASE("split preserves empty fields and returns views") {
    const std::string source = ",one,,two,";
    const auto parts = apex::split(source, ',');
    REQUIRE(parts.size() == 5);
    CHECK(parts[0].empty());
    CHECK(parts[1] == "one");
    CHECK(parts[2].empty());
    CHECK(parts[3] == "two");
    CHECK(parts[4].empty());
    CHECK(parts[1].data() == source.data() + 1);
    const auto empty = apex::split("", ',');
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].empty());
    const auto single = apex::split("whole", ',');
    REQUIRE(single.size() == 1);
    CHECK(single[0] == "whole");
}

TEST_CASE("trim and startsWith use exact ASCII rules") {
    CHECK(apex::trim(" \t\r\n\f\vcontent \t") == "content");
    CHECK(apex::trim(" \r\n\t").empty());
    CHECK(apex::trim("").empty());
    CHECK(apex::trim("in ner") == "in ner");
    CHECK(apex::trim("\xA0") == "\xA0");
    CHECK(apex::startsWith("material", "mat"));
    CHECK(apex::startsWith("material", ""));
    CHECK(apex::startsWith("", ""));
    CHECK_FALSE(apex::startsWith("", "a"));
    CHECK_FALSE(apex::startsWith("mat", "material"));
    CHECK_FALSE(apex::startsWith("material", "MAT"));
}
