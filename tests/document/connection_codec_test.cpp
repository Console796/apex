#include "document/connection_codec.hpp"

#include <doctest/doctest.h>

using namespace apex::document;

TEST_CASE("Connection codec preserves separators text and original bytes") {
    for (const char separator : {'\x1b', ','}) {
        const auto original = std::string("door") + separator + "Open" + separator + "" + separator + "0.50" + separator + "-1";
        auto connection = parseConnection(original);
        CHECK(connection.wellFormed);
        CHECK(connection.separator == separator);
        CHECK(connection.fields() == std::array<std::string, 5>{"door", "Open", "", "0.50", "-1"});
        CHECK(connection.raw == original);
        CHECK(serialize(connection) == original);
        connection.setField(ConnectionField::Delay, "1.25");
        CHECK(serialize(connection) == std::string("door") + separator + "Open" + separator + "" + separator + "1.25" + separator + "-1");
        connection.setField(ConnectionField::Delay, "0.50");
        CHECK(serialize(connection) == original);
        connection.target = "other";
        CHECK(serialize(connection) != original);
        connection.target = "door";
        CHECK(serialize(connection) == original);
    }
    Connection fresh;
    CHECK(fresh.separator == '\x1b');
    CHECK(serialize(fresh) == std::string(4, '\x1b'));
}

TEST_CASE("Connection codec malformed and empty fields round trip losslessly") {
    for (const auto& text : {std::string{}, std::string("zero separators"), std::string("a,b,c"),
                              std::string("a,b,c,d,e,f"), std::string("a\x1b" "b\x1b" "c"),
                              std::string("a\x1b" "b\x1b" "c\x1b" "d\x1b" "e\x1b" "f"),
                              std::string("a\0b,c,d", 7)}) {
        auto connection = parseConnection(text);
        CHECK_FALSE(connection.wellFormed);
        CHECK(connection.raw == text);
        CHECK(serialize(connection) == text);
        const auto target = connection.target;
        connection.setField(ConnectionField::Target, "changed");
        connection.setField(ConnectionField::Target, target);
        CHECK(serialize(connection) == text);
    }
    auto empty = parseConnection("a\x1b\x1b\x1b" "0\x1b-1");
    CHECK(empty.wellFormed);
    CHECK(empty.fields() == std::array<std::string, 5>{"a", "", "", "0", "-1"});
    auto mixed = parseConnection("a,b\x1b" "Open\x1b\x1b" "0\x1b-1");
    CHECK(mixed.wellFormed);
    CHECK(mixed.target == "a,b");
}
