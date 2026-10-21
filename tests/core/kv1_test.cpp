#include "core/kv1.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>

namespace {

using namespace apex::kv1;

std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::filesystem::path fixtures() {
    return std::filesystem::path(APEX_FIXTURE_DIR) / "kv1";
}

Document parsed(std::string source, EscapeMode mode = EscapeMode::Strata) {
    auto result = parse(std::move(source), mode);
    REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().what()));
    return std::move(*result);
}

std::string written(const Document& document) {
    const auto result = write(document);
    REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().what()));
    return *result;
}

} // namespace

TEST_CASE("KV1 fixtures round trip with both newline styles and escape profiles") {
    for (const auto& entry : std::filesystem::directory_iterator(fixtures())) {
        const auto source = read(entry.path());
        const auto mode = entry.path().filename() == "escapes_legacy.kv" ? EscapeMode::Legacy
                                                                       : EscapeMode::Strata;
        CAPTURE(entry.path().string());
        auto document = parsed(source, mode);
        CHECK_FALSE(document.root().modified());
        CHECK(written(document) == source);
        auto copy = document;
        document = Document{};
        CHECK(written(copy) == source);
    }
    CHECK(parsed(read(fixtures() / "duplicates_crlf.vmf")).lineEnding() == "\r\n");
    CHECK(parsed(read(fixtures() / "duplicates_lf.vmf")).lineEnding() == "\n");
    CHECK(Document{}.lineEnding() == "\r\n");
}

TEST_CASE("KV1 token spans partition the source including comments and trivia") {
    const std::string source = "// header\r\n\t\"key\" \"value\" [$WIN32]\r\nblock { k v }\n";
    auto document = parsed(source);
    std::size_t offset = 0;
    std::size_t line = 1;
    bool comment = false, whitespace = false, condition = false, brace = false;
    for (const auto& token : document.tokens()) {
        CHECK(token.span.offset == offset);
        CHECK(token.span.line == line);
        CHECK(token.span.length > 0);
        for (const char c : std::string_view(source).substr(offset, token.span.length)) {
            line += c == '\n' ? 1U : 0U;
        }
        offset += token.span.length;
        comment |= token.kind == TokenKind::Comment;
        whitespace |= token.kind == TokenKind::Whitespace;
        condition |= token.kind == TokenKind::Condition;
        brace |= token.kind == TokenKind::OpenBrace;
    }
    CHECK(offset == source.size());
    CHECK(comment);
    CHECK(whitespace);
    CHECK(condition);
    CHECK(brace);
    const auto& key = document.root().children()[0];
    CHECK(key.sourceSpan().offset == source.find("\"key\""));
    CHECK(key.sourceSpan().line == 2);
    CHECK(std::string_view(source).substr(key.sourceSpan().offset, key.sourceSpan().length) ==
          "\"key\" \"value\" [$WIN32]");
}

TEST_CASE("KV1 keeps all 49 ordered duplicate keys and case insensitive lookup") {
    auto document = parsed(read(fixtures() / "duplicates_lf.vmf"));
    auto* world = document.root().find("WORLD");
    REQUIRE(world);
    auto* editor = world->find("EdItOr");
    REQUIRE(editor);
    const auto all = editor->findAll("VISGROUPID");
    REQUIRE(all.size() == 49);
    CHECK(editor->find("visgroupid") == all[0]);
    CHECK(editor->find("missing") == nullptr);
    CHECK(editor->findAll("missing").empty());
    for (std::size_t i = 0; i < all.size(); ++i) {
        REQUIRE(all[i]->asInt());
        CHECK(*all[i]->asInt() == static_cast<int>(i + 1));
    }
    const auto& constant = std::as_const(*editor);
    CHECK(constant.findAll("visgroupid").size() == 49);
    CHECK(constant.find("visgroupid") == all[0]);
    CHECK_FALSE(editor->rawValue());
    CHECK_FALSE(editor->decodedValue());
}

TEST_CASE("KV1 raw escapes survive while decoded access honors the profile") {
    auto strata = parsed(read(fixtures() / "escapes_strata.kv"));
    auto* text = strata.root().find("text");
    REQUIRE(text);
    CHECK(text->rawValue() == "line\\nnext\\t\\\"quoted\\\"\\\\end\\q");
    CHECK(*text->decodedValue() == "line\nnext\t\"quoted\"\\end\\q");
    REQUIRE(text->setValue("new\n\t\"\\"));
    CHECK(text->rawValue() == "line\\nnext\\t\\\"quoted\\\"\\\\end\\q");
    auto roundtrip = parsed(written(strata));
    CHECK(*roundtrip.root().find("text")->decodedValue() == "new\n\t\"\\");
    auto legacy = parsed(read(fixtures() / "escapes_legacy.kv"), EscapeMode::Legacy);
    CHECK(*legacy.root().find("path")->decodedValue() == "C:\\new\\textures\\brick\\");
    auto unquoted = parsed("path C:\\new\\textures\n");
    CHECK(*unquoted.root().find("path")->decodedValue() == "C:\\new\\textures");
    unquoted.root().find("path")->setKey("renamed");
    CHECK(*parsed(written(unquoted)).root().find("renamed")->decodedValue() ==
          "C:\\new\\textures");
    const std::string both = "\"x\" \"a\\nb\\t\\\\\"\n";
    CHECK(written(parsed(both, EscapeMode::Legacy)) == both);
    CHECK(written(parsed(both, EscapeMode::Strata)) == both);
    CHECK(*parsed(both, EscapeMode::Legacy).root().find("x")->decodedValue() == "a\\nb\\t\\\\");
    auto doc = Document(EscapeMode::Legacy);
    REQUIRE(doc.root().append(Node::value("key", "literal\\n")));
    CHECK(written(doc) == "\"key\" \"literal\\n\"\r\n");
    REQUIRE(doc.root().children()[0].setValue("unrepresentable\""));
    CHECK_FALSE(write(doc));
}

TEST_CASE("KV1 typed accessors reject partial overflow and malformed numbers") {
    for (const auto value : {"0", "-23", "+42", " 42 \t", "2147483647", "-2147483648"}) {
        CHECK(Node::value("n", value).asInt());
    }
    for (const auto value : {"", " ", "1x", "1.5", "2147483648", "-2147483649", "+-1"}) {
        CHECK_FALSE(Node::value("n", value).asInt());
    }
    CHECK(*Node::value("n", "-1.25e+2").asDouble() == -125.0);
    for (const auto value : {"", "nan", "inf", "1e999", "1.5junk", "1 2"}) {
        CHECK_FALSE(Node::value("n", value).asDouble());
    }
    CHECK(*Node::value("v", "1 2 3").asVec3() == Vec3{1, 2, 3});
    CHECK(*Node::value("v", " [1\t2 3.5] ").asVec3() == Vec3{1, 2, 3.5});
    for (const auto value : {"1 2", "1 2 3 4", "[1 2 3", "1 2 3]", "1 nan 3"}) {
        CHECK_FALSE(Node::value("v", value).asVec3());
    }
    CHECK(*Node::value("c", "0 128 255").asColor255() == Color255{0, 128, 255, 255});
    CHECK(*Node::value("c", "1 2 3 4").asColor255() == Color255{1, 2, 3, 4});
    for (const auto value : {"1 2", "1 2 3 4 5", "-1 2 3", "256 2 3", "1 2 3.5"}) {
        CHECK_FALSE(Node::value("c", value).asColor255());
    }
}

TEST_CASE("KV1 conditionals are separate and survive mutation at either block position") {
    auto document = parsed(read(fixtures() / "gameinfo.txt"));
    const auto* gameinfo = document.root().find("gameinfo");
    REQUIRE(gameinfo);
    const auto* paths = gameinfo->find("FileSystem")->find("SearchPaths");
    REQUIRE(paths);
    CHECK(paths->children()[0].condition() == "[$WIN32]");
    CHECK(*paths->children()[0].decodedValue() == "|gameinfo_path|.");
    CHECK(paths->children()[1].condition() == "[!$WIN32]");
    CHECK(gameinfo->find("WindowsOnly")->condition() == "[$WIN32]");
    for (const auto source : {"block [$X] { k v }", "block { k v } [!$X]"}) {
        auto doc = parsed(source);
        const auto condition = std::string(doc.root().children()[0].condition());
        REQUIRE(doc.root().children()[0].children()[0].setValue("new"));
        auto again = parsed(written(doc));
        CHECK(again.root().children()[0].condition() == condition);
    }
    auto node = Node::value("k", "v");
    CHECK_FALSE(node.setCondition("[$]"));
    CHECK_FALSE(node.setCondition("[WIN32]"));
    CHECK_FALSE(node.setCondition("[$X] junk"));
    CHECK(node.setCondition("[!$X]"));
    CHECK(node.setCondition(""));
}

TEST_CASE("KV1 new nodes use Hammer formatting and CRLF by default") {
    Document document;
    auto block = Node::block("world");
    REQUIRE(block.append(Node::value("id", "1")));
    REQUIRE(block.append(Node::block("empty")));
    REQUIRE(document.root().append(std::move(block)));
    CHECK(written(document) == "\"world\"\r\n{\r\n\t\"id\" \"1\"\r\n\t\"empty\"\r\n"
                               "\t{\r\n\t}\r\n}\r\n");
    CHECK(parse(written(document)));
}

TEST_CASE("KV1 edits preserve untouched siblings raw spans and surrounding comments") {
    const std::string source = "// header\nworld\n{\n    \"change\"    \"old\"\n"
                               "\t// keep this comment\n    \"unknown\"   \"15.999900\""
                               " // tail\n}\n";
    auto document = parsed(source);
    auto* world = document.root().find("world");
    REQUIRE(world->find("change")->setValue("new"));
    const auto output = written(document);
    CHECK(output == "// header\nworld\n{\n\t\"change\" \"new\"\n"
                    "\t// keep this comment\n    \"unknown\"   \"15.999900\" // tail\n}\n");
    CHECK(parse(output));
    CHECK(document.source() == source);
    REQUIRE(world->insert(1, Node::value("inserted", "2")));
    REQUIRE(world->append(Node::value("last", "3")));
    REQUIRE(world->remove(0));
    auto again = parsed(written(document));
    const auto children = again.root().find("world")->children();
    REQUIRE(children.size() == 3);
    CHECK(children[0].key() == "inserted");
    CHECK(children[1].key() == "unknown");
    CHECK(children[2].key() == "last");
    CHECK(written(document).find("\"unknown\"   \"15.999900\"") != std::string::npos);
    CHECK_FALSE(world->insert(99, Node::block("bad")));
    CHECK_FALSE(world->remove(99));
    CHECK_FALSE(world->setValue("bad"));
    CHECK_FALSE(Node::value("k", "v").append(Node::value("k", "v")));
}

TEST_CASE("KV1 structural edits handle compact blocks empty blocks and moved nodes") {
    for (const auto source : {"block{}", "block {a b c d}", "block { a b\n}\n",
                              "block {\r\n\ta b\r\n}\r\n"}) {
        auto document = parsed(source);
        auto& block = document.root().children()[0];
        REQUIRE(block.insert(0, Node::value("first", "new")));
        REQUIRE(block.append(Node::value("last", "new")));
        CHECK(parse(written(document)));
        while (!block.children().empty()) {
            REQUIRE(block.remove(0));
        }
        CHECK(parse(written(document)));
    }
    auto document = parsed("a {\n k v\n}\nb {}\n");
    auto moved = document.root().children()[0];
    REQUIRE(document.root().children()[1].append(std::move(moved)));
    auto again = parsed(written(document));
    CHECK(*again.root().find("b")->find("a")->find("k")->decodedValue() == "v");
    auto key = Node::value("old", "v");
    key.setKey("new\"key");
    REQUIRE(document.root().append(std::move(key)));
    CHECK(parsed(written(document)).root().find("new\"key"));
}

TEST_CASE("KV1 directives resolve without changing the source document") {
    auto mixedCase = parsed("#incLude\"\""); // Minimized mutation-equivalence fuzz finding.
    REQUIRE(mixedCase.root().children()[0].setValue("changed"));
    const auto rewritten = parsed(written(mixedCase));
    CHECK(rewritten.root().children()[0].key() == "#incLude");
    mixedCase.root().children()[0].setKey("ordinary");
    CHECK(parsed(written(mixedCase)).root().children()[0].kind() == NodeKind::Value);
    const auto source = read(fixtures() / "directives.kv");
    const auto document = parsed(source);
    const auto directives = document.directives();
    REQUIRE(directives.size() == 2);
    CHECK(directives[0]->kind() == NodeKind::Base);
    CHECK(directives[1]->kind() == NodeKind::Include);
    CHECK(*directives[0]->decodedValue() == "base.kv");
    auto expanded = document.resolve([](std::string_view path) -> apex::Result<std::string> {
        return read(fixtures() / path);
    });
    REQUIRE_MESSAGE(expanded, (expanded ? "" : expanded.error().what()));
    CHECK(expanded->directives().empty());
    CHECK(expanded->root().findAll("settings").size() == 2);
    CHECK(*expanded->root().find("settings")->find("shared")->decodedValue() == "local wins");
    CHECK(*expanded->root().find("settings")->find("inherited")->decodedValue() == "yes");
    CHECK(expanded->root().find("from_include"));
    CHECK(expanded->root().findAll("duplicate").size() == 2);
    CHECK(parse(written(*expanded)));
    CHECK(written(document) == source);
    CHECK_FALSE(document.resolve({}));
    const auto missing = document.resolve([](std::string_view) -> apex::Result<std::string> {
        return std::unexpected(apex::Error{apex::ErrorCode::NotFound, "missing file"});
    });
    REQUIRE_FALSE(missing);
    CHECK(missing.error().what().find("base.kv") != std::string::npos);
    const auto malformed = document.resolve([](std::string_view) -> apex::Result<std::string> {
        return "broken {";
    });
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().what().find("line 1") != std::string::npos);
    const auto cyclic = document.resolve([](std::string_view) -> apex::Result<std::string> {
        return "#include \"base.kv\"";
    });
    REQUIRE_FALSE(cyclic);
    CHECK(cyclic.error().what().find("cycle") != std::string::npos);
    const auto nested = parsed("outer { #include \"other.kv\" }").resolve(
        [](std::string_view) -> apex::Result<std::string> { return "k v"; });
    REQUIRE(nested);
    CHECK(nested->root().find("outer")->find("k"));
}

TEST_CASE("KV1 malformed syntax returns contextual errors and nesting is bounded") {
    for (const auto source : {"\"unterminated", "a {", "a { b c", "}", "{ a b }", "key",
                              "k v stray", "k }", "k [$X] v", "k v [$]", "k v [$X",
                              "a [$X] {} [$Y]", "#base { k v }"}) {
        CAPTURE(source);
        const auto result = parse(source);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == apex::ErrorCode::ParseError);
        CHECK(result.error().what().find("line ") != std::string::npos);
        CHECK(result.error().what().find("column ") != std::string::npos);
    }
    const auto contextual = parse("k v\r\n  }");
    REQUIRE_FALSE(contextual);
    CHECK(contextual.error().what().find("line 2, column 3") != std::string::npos);
    std::string deep;
    for (int i = 0; i < 256; ++i) {
        deep += "k {";
    }
    deep.append(256, '}');
    CHECK(parse(deep));
    CHECK_FALSE(parse("k {" + deep + "}"));
    CHECK(written(parsed("// only trivia\r\n")) == "// only trivia\r\n");
    CHECK(written(parsed("")) == "");
    const std::string binary{"\"k\" \"a\0b\"", 9};
    CHECK(written(parsed(binary)) == binary);
}

TEST_CASE("KV1 optional loose P2CE corpus") {
    const auto* directory = std::getenv("APEX_P2CE_DIR");
    if (!directory || *directory == '\0') {
        std::cout << "KV1 corpus skipped: APEX_P2CE_DIR is unset\n";
        return;
    }
    const std::filesystem::path root(directory);
    if (!std::filesystem::is_directory(root)) {
        std::cout << "KV1 corpus skipped: APEX_P2CE_DIR is not a directory: " << root << '\n';
        return;
    }

    // Source's loose-data directories also contain several line-oriented formats that merely
    // share KV1's .txt extension. Keep these exceptions explicit so newly parseable files and
    // newly encountered non-KV1 formats both make the test fail.
    const std::array<std::string_view, 14> expectedNonKv1{
        "scripts/sentences/sentences_hl2.txt",
        "scripts/soundmixers.txt",
        "scripts/dsp_presets.txt",
        "scripts/titles.txt",
        "scripts/hudanimations.txt",
        "scripts/gameuianimations.txt",
        "scripts/talker/response_criteria.txt",
        "media/startupvids.txt",
        "scripts/plugin_animations.txt",
        "resource/fonts/FontCredits.txt",
        "scripts/bugreporter_defaults.txt",
        "panorama/localization/panorama_english.txt",
        "scripts/talker/eggbot.txt",
        "scripts/talker/ballbot.txt",
    };
    std::array<bool, expectedNonKv1.size()> expectedSeen{};
    std::size_t scanned = 0;
    std::size_t strataCount = 0;
    std::size_t legacyCount = 0;
    std::size_t skipped = 0;
    std::size_t expectedFailures = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto extension = entry.path().extension();
        if (extension != ".txt" && extension != ".kv" && extension != ".vmt" &&
            extension != ".vmf") {
            continue;
        }
        ++scanned;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        const auto firstSeparator = relative.find('/');
        const auto topLevel = relative.substr(0, firstSeparator);
        if (topLevel == "bin" || topLevel == "sdk_tools" ||
            entry.path().filename() == "THIRD-PARTY-LEGAL-NOTICES.txt") {
            ++skipped;
            continue;
        }
        const auto source = read(entry.path());
        if (source.starts_with("\xFF\xFE") || source.starts_with("\xFE\xFF")) {
            ++skipped;
            continue;
        }

        std::optional<std::size_t> expectedIndex;
        for (std::size_t i = 0; i < expectedNonKv1.size(); ++i) {
            if (relative.ends_with(expectedNonKv1[i])) {
                expectedIndex = i;
                expectedSeen[i] = true;
                break;
            }
        }

        CAPTURE(relative);
        auto document = parse(source, EscapeMode::Strata);
        if (document) {
            CHECK_FALSE(expectedIndex.has_value());
            REQUIRE(written(*document) == source);
            ++strataCount;
            continue;
        }
        const auto strataError = document.error().what();
        document = parse(source, EscapeMode::Legacy);
        if (document) {
            CHECK_FALSE(expectedIndex.has_value());
            REQUIRE(written(*document) == source);
            ++legacyCount;
            continue;
        }
        if (expectedIndex) {
            ++expectedFailures;
            continue;
        }
        INFO("not KV1 in Strata or Legacy mode; Strata: ", strataError,
             "; Legacy: ", document.error().what());
        CHECK(false);
    }
    for (std::size_t i = 0; i < expectedNonKv1.size(); ++i) {
        CAPTURE(expectedNonKv1[i]);
        CHECK(expectedSeen[i]);
    }
    std::cout << "KV1 corpus: scanned=" << scanned << ", parsed Strata=" << strataCount
              << ", parsed Legacy=" << legacyCount << ", skipped=" << skipped
              << ", expected failures=" << expectedFailures << '\n';
    CHECK(scanned > 0);
}
