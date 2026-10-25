#include "core/kv3.hpp"

#include <doctest/doctest.h>
#include <algorithm>

using namespace apex;
namespace {
constexpr std::string_view header = "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n";
void spans(const kv3::Value& value, std::string_view source) {
    auto span = value.span();
    REQUIRE(span.offset + span.length <= source.size());
    CHECK(span.length > 0);
    CHECK(span.line == 1 + static_cast<std::size_t>(std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(span.offset), '\n')));
    auto fragment = source.substr(span.offset, span.length);
    auto reparsed = kv3::parse(std::string(header) + std::string(fragment));
    REQUIRE_MESSAGE(reparsed, fragment);
    CHECK(reparsed->root.kind() == value.kind());
    CHECK(reparsed->root.flag == value.flag);
    CHECK(reparsed->root.span().length == span.length);
    if (auto array = value.get<kv3::Value::Array>())
        for (const auto& child : *array) spans(child, source);
    if (auto object = value.get<kv3::Value::Object>())
        for (const auto& member : *object) {
            const auto rawKey = source.substr(member.keySpan.offset, member.keySpan.length);
            CHECK_FALSE(rawKey.empty());
            CHECK((rawKey == member.key || rawKey == "\"quoted key\""));
            spans(member.value, source);
        }
}
}

TEST_CASE("kv3 text values preserve spans flags and ordered duplicate keys") {
    const std::string source = std::string(header) + R"kv3({
    // Apex-authored fixture
    nested = { key.name = [ -42 1.25, -2e+3, true false null, ] }
    duplicate = "first"
    duplicate = "second" /* keep both */
    "quoted key" = "quote\" slash\\ newline\n tab\t cr\r apostrophe\' question\?"
    multiline = """
Apex text
second line
"""
    flagged = [ resource:"x" soundevent:"y" ]
    bytes = #[ 01 02 ff ]
})kv3";
    auto doc = kv3::parse(source);
    REQUIRE_MESSAGE(doc, (doc ? "" : doc.error().what()));
    CHECK(doc->encoding == "text");
    CHECK(doc->format == "generic");
    CHECK(*doc->root.find("duplicate")->get<std::string>() == "first");
    CHECK(doc->root.get<kv3::Value::Object>()->size() == 7);
    CHECK(*doc->root.find("multiline")->get<std::string>() == "Apex text\nsecond line");
    CHECK(*doc->root.find("quoted key")->get<std::string>() == "quote\" slash\\ newline\n tab\t cr\r apostrophe\' question\?");
    auto array = doc->root.find("nested")->find("key.name")->get<kv3::Value::Array>();
    REQUIRE(array);
    CHECK(*(*array)[0].get<std::int64_t>() == -42);
    CHECK(*(*array)[1].get<double>() == 1.25);
    CHECK(*(*array)[2].get<double>() == -2000);
    CHECK(*(*array)[3].get<bool>());
    CHECK_FALSE(*(*array)[4].get<bool>());
    CHECK((*array)[5].kind() == kv3::Value::Kind::Null);
    CHECK(*doc->root.find("bytes")->get<kv3::Value::Binary>() == kv3::Value::Binary{1, 2, 255});
    CHECK((*doc->root.find("flagged")->get<kv3::Value::Array>())[0].flag == "resource");
    spans(doc->root, source);
}

TEST_CASE("kv3 malformed text reports lines and bounds nesting") {
    for (const auto* input : {"{ a = 1 }", "<!-- kv3 encoding:binary:version{x} format:generic:version{x} --> {}"}) {
        auto doc = kv3::parse(input);
        REQUIRE_FALSE(doc);
        CHECK(doc.error().message.find("line 1") != std::string::npos);
    }
    for (const auto* body : {"{ a = \"unterminated", "{ a = [ 1 }", "{ a = /* open", "{ a = 1e }",
                             "{ a = #[ gg ] }", "{ a = 9223372036854775808 }", "{ a = \"\\q\" }",
                             "{ a = \"\"\"same line\"\"\" }", "{ a = 1e999 }"}) {
        auto doc = kv3::parse(std::string(header) + body);
        REQUIRE_FALSE(doc);
        CHECK(doc.error().message.find("line 2") != std::string::npos);
    }
    auto deep = kv3::parse(std::string(header) + std::string(300, '[') + "0" + std::string(300, ']'));
    REQUIRE_FALSE(deep);
    CHECK(deep.error().message.find("depth limit") != std::string::npos);
    CHECK(deep.error().message.find("line 2") != std::string::npos);
    auto crlf = kv3::parse(std::string(header) + "{ s = \"\"\"\r\nline\r\n\"\"\"\r\n}");
    REQUIRE(crlf);
    CHECK(*crlf->root.find("s")->get<std::string>() == "line");
}
