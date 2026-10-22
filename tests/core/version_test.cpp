#include <doctest/doctest.h>

#include <string>

#include "core/version.hpp"

TEST_CASE("version string matches its components") {
    const auto& v = apex::version();
    const std::string expected = std::to_string(v.major) + "." + std::to_string(v.milestone) + "." +
                                 std::to_string(v.sub) + "." + std::to_string(v.build) +
                                 (v.release ? "-release" : "");
    CHECK(std::string(v.string) == expected);
    CHECK(v.major == 0);
    CHECK_FALSE(v.gitHash.empty());
}
