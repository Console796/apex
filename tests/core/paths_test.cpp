#include "core/paths.hpp"

#include <doctest/doctest.h>

#include <array>
#include <string_view>

TEST_CASE("Source path normalization handles separators roots and dot components") {
    struct Example {
        std::string_view input;
        std::string_view expected;
    };
    constexpr std::array examples{
        Example{"a\\b//c/../d", "a/b/d"},
        Example{"./x", "x"},
        Example{"../x", "../x"},
        Example{"/..", "/"},
        Example{"a/./b/", "a/b"},
        Example{"", ""},
        Example{".", ""},
        Example{"././.", ""},
        Example{"..", ".."},
        Example{"../..", "../.."},
        Example{"...", "..."},
        Example{"....", "...."},
        Example{"a/..", ""},
        Example{"a/../../x", "../x"},
        Example{"../../a/../b", "../../b"},
        Example{"/a/../../../x/", "/x"},
        Example{"///", "/"},
        Example{"\\", "/"},
        Example{"\\A\\B\\..\\C\\", "/A/C"},
        Example{"a/.../..", "a"},
        Example{"a/..b/.hidden", "a/..b/.hidden"},
        Example{" name / X ", " name / X "},
    };
    for (const auto& [input, expected] : examples) {
        CAPTURE(input);
        const auto normalized = apex::normalizePath(input);
        CHECK(normalized == expected);
        CHECK(apex::normalizePath(normalized) == normalized);
    }
    CHECK(apex::lookupKey("Materials\\BRICK//./Wall.VMT") == "materials/brick/wall.vmt");
    CHECK(apex::lookupKey("A/../B/FILE.TXT/") == "b/file.txt");
    CHECK(apex::lookupKey("../MiXeD") == "../mixed");
    CHECK(apex::lookupKey("").empty());
}

TEST_CASE("extensions and stems use the normalized final component") {
    CHECK(apex::extension("a\\b.vmt") == ".vmt");
    CHECK(apex::stem("a\\b.vmt") == "b");
    CHECK(apex::extension("a.b/c").empty());
    CHECK(apex::stem("a.b/c") == "c");
    CHECK(apex::extension("a.tar.gz/") == ".gz");
    CHECK(apex::stem("a.tar.gz/") == "a.tar");
    CHECK(apex::extension(".config").empty());
    CHECK(apex::stem(".config") == ".config");
    CHECK(apex::extension(".config.json") == ".json");
    CHECK(apex::stem(".config.json") == ".config");
    CHECK(apex::extension("file.") == ".");
    CHECK(apex::stem("file.") == "file");
    CHECK(apex::extension("../..").empty());
    CHECK(apex::stem("../..") == "..");
    CHECK(apex::extension("a.txt/..").empty());
    CHECK(apex::stem("a.txt/..").empty());
    for (auto path : {"", "/", ".", "./"}) {
        CHECK(apex::extension(path).empty());
        CHECK(apex::stem(path).empty());
    }
}

TEST_CASE("joining paths normalizes across the join boundary") {
    CHECK(apex::join("a/b", "../c") == "a/c");
    CHECK(apex::join("a\\b/", ".\\c") == "a/b/c");
    CHECK(apex::join("a", "/B") == "/B");
    CHECK(apex::join("a", "\\B") == "/B");
    CHECK(apex::join("", "x") == "x");
    CHECK(apex::join("x/", "") == "x");
    CHECK(apex::join("", "").empty());
    CHECK(apex::join("/", "../../x") == "/x");
    CHECK(apex::join("..", "../x") == "../../x");
}
