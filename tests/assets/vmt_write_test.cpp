#include "assets/material_index.hpp"
#include "assets/vmt.hpp"
#include "assets/vmt_write.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::assets;

TEST_CASE("editVmt sets, adds and removes top-level parameters and keeps the rest verbatim") {
    const std::string source =
        "// header comment\r\n\"LightmappedGeneric\"\r\n{\r\n\t\"$basetexture\" \"dev/white\"\r\n"
        "\t\"$SurfaceProp\"   \"concrete\"   // old\r\n\t\"Proxies\" { \"Sine\" { \"resultVar\" \"$alpha\" } }\r\n}\r\n";
    const std::vector<VmtEdit> edits{{"$surfaceprop", "metal"}, {"$color", "[1 0 0]"}, {"$basetexture", std::nullopt}};
    auto result = editVmt(source, edits);
    REQUIRE(result);
    CHECK(result->find("// header comment") != std::string::npos);
    CHECK(result->find("\"$SurfaceProp\" \"metal\"") != std::string::npos); // key spelling kept, value replaced
    CHECK(result->find("\r\n") != std::string::npos);                       // line endings kept
    CHECK(result->find("// old") != std::string::npos);
    CHECK(result->find("$basetexture") == std::string::npos);
    CHECK(result->find("\"$color\"") != std::string::npos);
    CHECK(result->find("\"resultVar\" \"$alpha\"") != std::string::npos);
    // An edit that changes nothing writes the same text back.
    const std::vector<VmtEdit> none;
    auto same = editVmt(source, none);
    REQUIRE(same);
    CHECK(*same == source);
    // Round trip through the material parser.
    auto reread = [&](const std::string& text) {
        return indexMaterial([&](std::string_view) -> Result<std::string> { return text; }, "test/mat", std::nullopt);
    };
    CHECK(reread(source).surfaceProp == "concrete");
    CHECK(reread(*result).surfaceProp == "metal");
}

TEST_CASE("editVmt rejects bad keys, values and non-VMT text without producing partial output") {
    const std::string source = "\"UnlitGeneric\" { \"$basetexture\" \"a\" }";
    CHECK(!editVmt(source, std::vector<VmtEdit>{{"", "x"}}));
    CHECK(!editVmt(source, std::vector<VmtEdit>{{"two words", "x"}}));
    CHECK(!editVmt(source, std::vector<VmtEdit>{{"$a", "quote\"inside"}}));
    CHECK(!editVmt(source, std::vector<VmtEdit>{{"$a", "line\nbreak"}}));
    CHECK(!editVmt("not a vmt {", std::vector<VmtEdit>{{"$a", "x"}}));
    CHECK(!editVmt("\"A\" { } \"B\" { }", std::vector<VmtEdit>{{"$a", "x"}}));
}

TEST_CASE("a snapshot copy replaces or adds one entry and stays sorted") {
    MaterialIndexEntry b, c, replacement;
    b.name = "b/b"; c.name = "c/c"; replacement.name = "B/B"; replacement.surfaceProp = "wood";
    MaterialIndexSnapshot base({c, b});
    auto changed = base.with(replacement);
    REQUIRE(changed->entries.size() == 2);
    CHECK(changed->find("b/b")->surfaceProp == "wood");
    CHECK(base.find("b/b")->surfaceProp.empty()); // the original is untouched
    MaterialIndexEntry a; a.name = "a/a";
    auto added = changed->with(a);
    REQUIRE(added->entries.size() == 3);
    CHECK(added->entries.front().name == "a/a");
}

#include "fixture.hpp"
#include "provider_fixture.hpp"

TEST_CASE("writeVmt writes loose files only, atomically, and the read-back sees the change") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", R"(GameInfo { FileSystem { SearchPaths { game "|gameinfo_path|." } } })");
    tree.put("Game/materials/loose.vmt", "\"LightmappedGeneric\" { \"$basetexture\" \"a\" }");
    JobPool jobs(2);
    GameProfile profile;
    profile.gameDirectory = tree.root / "Game";
    auto provider = std::make_shared<MemoryProvider>("p");
    provider->contents = {{"materials/served.vmt", "\"UnlitGeneric\" { }"}};
    auto vfs = Vfs::load(profile, jobs, {}, {{provider, {VfsPosition::Kind::Last, {}}}});
    REQUIRE(vfs);
    const std::vector<VmtEdit> edits{{"$surfaceprop", "glass"}};
    auto written = writeVmt(*vfs, "Loose", edits);
    REQUIRE(written);
    CHECK(*written == tree.root / "Game/materials/loose.vmt");
    CHECK(!std::filesystem::exists(tree.root / "Game/materials/loose.vmt.apex-tmp"));
    auto reread = loadMaterial(*vfs, "loose");
    REQUIRE(reread);
    CHECK(reread->surfaceProp == "glass");
    CHECK(reread->baseTexture == "a");
    auto fromProvider = writeVmt(*vfs, "served", edits);
    REQUIRE(!fromProvider);
    CHECK(fromProvider.error().code == ErrorCode::Unsupported);
    auto missing = writeVmt(*vfs, "nothing/here", edits);
    REQUIRE(!missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
}
