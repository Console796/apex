#include "core/result.hpp"

#include <doctest/doctest.h>

#include <memory>

TEST_CASE("Result holds values void success and errors") {
    const apex::Result<int> value = 42;
    REQUIRE(value.has_value());
    CHECK(*value == 42);
    const apex::Result<void> success;
    CHECK(success.has_value());
    apex::Result<std::unique_ptr<int>> owned = std::make_unique<int>(7);
    CHECK(**owned == 7);
    const apex::Error error{apex::ErrorCode::NotFound, "missing material"};
    const apex::Result<int> failure = std::unexpected(error);
    REQUIRE_FALSE(failure.has_value());
    CHECK(failure.error().code == apex::ErrorCode::NotFound);
    CHECK(failure.error().what() == "missing material");
    const apex::Result<void> voidFailure = std::unexpected(error);
    CHECK_FALSE(voidFailure.has_value());
}

TEST_CASE("Error contexts render outermost first without mutating the original") {
    const apex::Error error{apex::ErrorCode::ParseError, "invalid number", {}};
    const auto contextual = error.withContext("key origin").withContext("entity 12");
    CHECK(contextual.what() == "entity 12: key origin: invalid number");
    CHECK(contextual.code == apex::ErrorCode::ParseError);
    CHECK(contextual.message == "invalid number");
    CHECK(contextual.context.size() == 2);
    CHECK(error.what() == "invalid number");
    CHECK(error.context.empty());
    CHECK(contextual.withContext("map.vmf").what() ==
          "map.vmf: entity 12: key origin: invalid number");
}
