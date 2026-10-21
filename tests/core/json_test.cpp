#include "core/json.hpp"

#include <doctest/doctest.h>

using namespace apex;

TEST_CASE("JSON codec validates grammar and decodes Unicode escapes") {
    auto value = json::parse(R"({"s":"\u0041\ud83d\ude00","a":[null,true,false,-1.25e2]})");
    REQUIRE(value);
    auto object = value->get<json::Value::Object>();
    REQUIRE(object);
    CHECK(*object->at("s").get<std::string>() == "A\xf0\x9f\x98\x80");
    auto again = json::parse(json::write(*value));
    REQUIRE(again);
    CHECK(json::write(*again) == json::write(*value));
    for (auto invalid : {"", "[1,]", "{\"x\":1,}", "01", "1.", "+1", "1e", "1e9999",
                         "[true false]", "null extra", "\"\\uD800\"", "\"\\uDC00\"",
                         "\"\\q\"", "{\"a\":1,\"a\":2}"}) {
        INFO(invalid);
        CHECK_FALSE(json::parse(invalid));
    }
    CHECK_FALSE(json::parse(std::string(130, '[') + "0" + std::string(130, ']')));
}
