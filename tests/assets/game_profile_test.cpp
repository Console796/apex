#include "assets/game_profile.hpp"
#include "fixture.hpp"

#include <doctest/doctest.h>
#include <sourcepp/Math.h>
#include <steampp/steampp.h>

using namespace apex;
using namespace apex::assets;

TEST_CASE("GameProfile JSON round trips paths options and escaping") {
    GameTree tree;
    GameProfile profile;
    profile.name = "P2CE \"test\"\n";
    profile.gameDirectory = tree.root / "game";
    profile.fgds = {"base.fgd", "dir/extra.fgd"};
    profile.toolPaths = {{"vbsp", "C:\\tools\\vbsp.exe"}, {"vrad", "bin/vrad"}};
    profile.escapeMode = kv1::EscapeMode::Legacy;
    profile.vmfWrite = {false, false};
    profile.addons = {AddonSettings::List::Apex, false, {"addon/a", "addon/b"}};
    auto decoded = parseGameProfile(writeGameProfile(profile));
    REQUIRE(decoded);
    CHECK(*decoded == profile);
    REQUIRE(saveGameProfile(profile, tree.root / "profile.json"));
    auto loaded = loadGameProfile(tree.root / "profile.json");
    REQUIRE(loaded);
    CHECK(*loaded == profile);
    CHECK_FALSE(loadGameProfile(tree.root / "missing.json"));
    CHECK_FALSE(saveGameProfile(profile, tree.root / "missing/profile.json"));
    CHECK_FALSE(parseGameProfile("{}"));
    CHECK_FALSE(parseGameProfile(R"({"name":"test","game_directory":"g","kv_escape_mode":"unknown"})"));
    CHECK_FALSE(parseGameProfile(R"({"name":"test","game_directory":"g","vmf_write":{"markers":1}})"));
    auto defaults = parseGameProfile(R"({"name":"test","game_directory":"g","future":null})");
    REQUIRE(defaults);
    CHECK(defaults->addons.list == AddonSettings::List::Game);
    CHECK(defaults->addons.enabled);
    CHECK(defaults->addons.apexMounted.empty());
    for (const auto list : {AddonSettings::List::Hammer, AddonSettings::List::Game, AddonSettings::List::Apex}) {
        profile.addons.list = list;
        auto roundTrip = parseGameProfile(writeGameProfile(profile));
        REQUIRE(roundTrip);
        CHECK(*roundTrip == profile);
    }
    for (const auto* options : {"null", "[]", "{\"enabled\":1}", "{\"list\":true}",
                                "{\"list\":\"invalid\"}", "{\"mounted\":false}", "{\"mounted\":[1]}"}) {
        CHECK_FALSE(parseGameProfile(std::string(R"({"name":"test","game_directory":"g","addons":)") + options + "}"));
    }
    CHECK(defaults->vmfWrite.pointData);
    CHECK(defaults->escapeMode == kv1::EscapeMode::Strata);
}

TEST_CASE("P2CE profile is discovered through Steam") {
    steampp::Steam steam;
    if (!steam.isAppInstalled(440000)) {
        MESSAGE("Skipped: Steam has no installed P2CE app");
        return;
    }
    auto profile = detectP2ce();
    REQUIRE(profile);
    CHECK(profile->gameDirectory == steam.getAppInstallDir(440000) / "p2ce");
    CHECK(std::filesystem::is_regular_file(profile->gameDirectory / "gameinfo.txt"));
    CHECK_FALSE(profile->fgds.empty());
    CHECK(profile->toolPaths.contains("vbsp"));
}

TEST_CASE("GameProfile instance search root prefers the SDK source maps beside the install") {
    GameTree tree;
    GameProfile profile;
    profile.gameDirectory = tree.root / "install" / "p2ce";
    CHECK(profile.instanceSearchRoot() == profile.gameDirectory / "maps");
    std::filesystem::create_directories(tree.root / "install" / "sdk_content" / "maps");
    CHECK(profile.instanceSearchRoot() == tree.root / "install" / "sdk_content" / "maps");
}

TEST_CASE("GameProfile compile tools use platform paths and explicit overrides") {
    GameProfile profile;
    profile.gameDirectory = std::filesystem::path("install") / "p2ce";
#ifdef _WIN32
    const auto bin = std::filesystem::path("install/bin/win64");
    const std::string suffix = ".exe";
    CHECK(profile.toolPath("game") == bin / "p2ce.exe");
    CHECK(profile.toolPath("postcompiler") == bin / "postcompiler/postcompiler.exe");
#else
    const auto bin = std::filesystem::path("install/bin/linux64");
    const std::string suffix;
    CHECK(profile.toolPath("game") == std::filesystem::path("install/p2ce.sh"));
    CHECK(profile.toolPath("postcompiler") == bin / "postcompiler");
#endif
    CHECK(profile.toolDirectory() == bin);
    for (const auto* name : {"vbsp", "vvis", "vrad", "bspzip", "vbspinfo"})
        CHECK(profile.toolPath(name) == bin / (std::string(name) + suffix));
    profile.toolPaths["vbsp"] = "custom/compiler";
    profile.toolPaths["game"] = "custom/game";
    CHECK(profile.toolPath("vbsp") == std::filesystem::path("custom/compiler"));
    CHECK(profile.toolPath("game") == std::filesystem::path("custom/game"));
}

TEST_CASE("GameProfile tool_directory is optional and moves the tool fallbacks") {
    GameProfile profile;
    profile.name = "x";
    profile.gameDirectory = "/games/install/mod";
    CHECK(writeGameProfile(profile).find("tool_directory") == std::string::npos);
    profile.customToolDirectory = "/games/install/bin";
    auto decoded = parseGameProfile(writeGameProfile(profile));
    REQUIRE(decoded);
    CHECK(*decoded == profile);
    CHECK(decoded->toolDirectory() == std::filesystem::path("/games/install/bin"));
#ifdef _WIN32
    CHECK(decoded->toolPath("vbsp") == std::filesystem::path("/games/install/bin/vbsp.exe"));
#else
    CHECK(decoded->toolPath("vbsp") == std::filesystem::path("/games/install/bin/vbsp"));
#endif
    CHECK_FALSE(parseGameProfile(R"({"name":"x","game_directory":"g","tool_directory":3})"));
}

TEST_CASE("suggestGameProfile reads a Valve-layout install") {
    GameTree tree;
    tree.put("install/hl2/gameinfo.txt",
             "\"GameInfo\"\n{\n game \"Half-Life 2\"\n FileSystem\n {\n  SearchPaths\n  {\n   game |gameinfo_path|.\n  }\n }\n}\n");
#ifdef _WIN32
    for (auto tool : {"vbsp.exe", "vvis.exe", "vrad.exe"}) tree.put(std::string("install/bin/") + tool, "");
    tree.put("install/hl2.exe", "");
#else
    for (auto tool : {"vbsp", "vvis", "vrad"}) tree.put(std::string("install/bin/") + tool, "");
    tree.put("install/hl2.sh", "");
#endif
    tree.put("install/bin/halflife2.fgd", "");
    tree.put("install/bin/base.fgd", "");
    auto profile = suggestGameProfile(tree.root / "install/hl2/gameinfo.txt");
    REQUIRE(profile);
    CHECK(profile->name == "Half-Life 2");
    CHECK(profile->gameDirectory == tree.root / "install/hl2");
    CHECK(profile->customToolDirectory == tree.root / "install/bin");
    CHECK(profile->toolPaths.size() == 4);
    CHECK(profile->toolPaths.at("vrad").parent_path() == tree.root / "install/bin");
    CHECK(profile->toolPaths.contains("game"));
    REQUIRE(profile->fgds.size() == 2);
    CHECK(profile->fgds[0].filename() == "base.fgd");
    CHECK(profile->escapeMode == kv1::EscapeMode::Legacy);
    CHECK_FALSE(profile->vmfWrite.pointData);
    // The suggestion round-trips through the profile file unchanged.
    auto decoded = parseGameProfile(writeGameProfile(*profile));
    REQUIRE(decoded);
    CHECK(*decoded == *profile);
}

TEST_CASE("suggestGameProfile recognises Strata and refuses folders without gameinfo") {
    GameTree tree;
    tree.put("install/mymod/gameinfo.txt",
             "GameInfo\n{\n FileSystem\n {\n  SearchPaths\n  {\n   game |gameinfo_path|.\n  }\n }\n}\n");
    tree.put("install/mymod/cfg/mounts.kv", "Mounts {}\n");
    tree.put("install/mymod/mymod.fgd", "");
    auto profile = suggestGameProfile(tree.root / "install/mymod");
    REQUIRE(profile);
    CHECK(profile->name == "mymod");
    CHECK(profile->escapeMode == kv1::EscapeMode::Strata);
    CHECK(profile->customToolDirectory.empty());
    REQUIRE(profile->fgds.size() == 1);
    CHECK(profile->fgds[0].filename() == "mymod.fgd");
    CHECK_FALSE(suggestGameProfile(tree.root / "install"));
    tree.put("broken/gameinfo.txt", "GameInfo { game x }\n");
    CHECK_FALSE(suggestGameProfile(tree.root / "broken"));
}

TEST_CASE("gameProfileFileName keeps names readable and file-safe") {
    CHECK(gameProfileFileName("Half-Life 2") == "Half-Life 2.json");
    CHECK(gameProfileFileName("a/b:c*?") == "a_b_c__.json");
    CHECK(gameProfileFileName(" .. ") == "game.json");
}
