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
