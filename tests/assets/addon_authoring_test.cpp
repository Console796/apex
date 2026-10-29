#include "assets/addon_authoring.hpp"
#include "assets/vfs.hpp"
#include "core/kv3.hpp"
#include "fixture.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <fstream>
#include <iterator>

using namespace apex;
TEST_CASE("addon authoring creates uploader-shaped manifests without replacing existing files") {
    GameTree tree;
    const auto path = tree.root / "my addon";
    REQUIRE(assets::createAddon(path, {"A \"quoted\" name", true}));
    CHECK(std::filesystem::is_directory(path / "mapsrc/instances"));
    CHECK(std::filesystem::is_directory(path / "maps"));
    std::ifstream file(path / "addon.kv3");
    const std::string bytes(std::istreambuf_iterator<char>{file}, {});
    auto parsed = kv3::parse(bytes);
    REQUIRE(parsed);
    CHECK(*parsed->root.find("mod")->get<std::string>() == "A \"quoted\" name");
    CHECK(parsed->root.find("dependencies")->get<kv3::Value::Array>()->empty());
    auto campaign = assets::addonCampaign(path);
    REQUIRE(campaign);
    CHECK(*campaign == "addon:my addon/main");
    CHECK_FALSE(assets::addonCampaign(path, "absent"));
    CHECK_FALSE(assets::createAddon(path, {"replacement", false}));
    std::ifstream after(path / "addon.kv3");
    CHECK(std::string(std::istreambuf_iterator<char>{after}, {}) == bytes);
    REQUIRE(assets::createAddon(tree.root / "map", {"Map", false}));
    CHECK(assets::addonCampaign(tree.root / "map")->empty());
    tree.put("map/scripts/campaigns.kv3", "{campaigns={first={} second={}}}");
    CHECK_FALSE(assets::addonCampaign(tree.root / "map"));
    auto second = assets::addonCampaign(tree.root / "map", "second");
    REQUIRE(second);
    CHECK(*second == "addon:map/second");
}
TEST_CASE("addon mount comparison includes missing extra and precedence differences") {
    assets::AddonList editor, game;
    for (const auto* name : {"First", "Second"}) {
        assets::AddonEntry entry;
        entry.dir = name;
        entry.mounted = true;
        editor.entries.push_back(entry);
    }
    game = editor;
    CHECK(assets::addonMountDifference(editor, game).empty());
    std::swap(game.entries[0], game.entries[1]);
    CHECK(assets::addonMountDifference(editor, game).size() == 1);
    game.entries.pop_back();
    CHECK(assets::addonMountDifference(editor, game)[0] == "Editor only: First");
    CHECK(assets::addonMountDifference(game, editor)[0] == "Game only: First");
}
TEST_CASE("addon authoring rejects non-portable names and missing parents without touching the disk") {
    GameTree tree;
    for (const auto* bad : {".", "..", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "a\\b", "trailing.", "trailing ", "CON", "nul", "Com1", "aux.txt"}) {
        auto result = assets::createAddon(tree.root / bad, {"Name", false});
        CHECK_MESSAGE(!result, bad);
    }
    CHECK_FALSE(assets::createAddon(tree.root / "x" / "..", {"Name", false}));
    CHECK_FALSE(assets::createAddon(tree.root / "folder/", {"Name", false}));
    CHECK_FALSE(assets::createAddon({}, {"Name", false}));
    CHECK_FALSE(assets::createAddon(tree.root / "ok", {"", false}));
    CHECK_FALSE(std::filesystem::exists(tree.root / "ok"));
    CHECK_FALSE(std::filesystem::exists(tree.root / "trailing."));
    auto missing = assets::createAddon(tree.root / "absent" / "addon", {"Name", false});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(std::string(missing.error().what()).find("Parent folder does not exist") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(tree.root / "absent"));
}
TEST_CASE("addon authoring refuses an existing folder or file and leaves its content alone") {
    GameTree tree;
    tree.put("taken/keep.txt", "mine");
    auto merged = assets::createAddon(tree.root / "taken", {"Name", true});
    REQUIRE_FALSE(merged);
    CHECK(std::filesystem::exists(tree.root / "taken/keep.txt"));
    CHECK_FALSE(std::filesystem::exists(tree.root / "taken/addon.kv3"));
    CHECK_FALSE(std::filesystem::exists(tree.root / "taken/maps"));
    tree.put("file", "data");
    CHECK_FALSE(assets::createAddon(tree.root / "file", {"Name", false}));
    CHECK(std::filesystem::is_regular_file(tree.root / "file"));
}
TEST_CASE("addon campaign resolution reports missing files bad ids and malformed scripts") {
    GameTree tree;
    REQUIRE(assets::createAddon(tree.root / "map", {"Map", false}));
    auto missing = assets::addonCampaign(tree.root / "map", "main");
    REQUIRE_FALSE(missing);
    CHECK(std::string(missing.error().what()).find("Campaigns file is missing") != std::string::npos);
    CHECK(assets::addonCampaign(tree.root / "map")->empty());
    tree.put("multi/addon.kv3", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{ mod = "Multi" })");
    tree.put("multi/scripts/campaigns.kv3", "{campaigns={first={} second={}}}");
    auto ambiguous = assets::addonCampaign(tree.root / "multi");
    REQUIRE_FALSE(ambiguous);
    CHECK(ambiguous.error().code == ErrorCode::InvalidArgument);
    CHECK(std::string(ambiguous.error().what()).find("campaign ID") != std::string::npos);
    CHECK(*assets::addonCampaign(tree.root / "multi", "first") == "addon:multi/first");
    auto bad = assets::addonCampaign(tree.root / "multi", "third");
    REQUIRE_FALSE(bad);
    CHECK(std::string(bad.error().what()).find("Unknown campaign") != std::string::npos);
    for (const auto* id : {"first/../second", "a\\b", "../first", "first/"})
        CHECK_MESSAGE(!assets::addonCampaign(tree.root / "multi", id), id);
    tree.put("broken/addon.kv3", "<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->\n{ mod = \"Broken\" }");
    tree.put("broken/scripts/campaigns.kv3", "this is { not kv3");
    CHECK_FALSE(assets::addonCampaign(tree.root / "broken"));
    tree.put("broken/scripts/campaigns.kv3", "{ other = {} }");
    CHECK_FALSE(assets::addonCampaign(tree.root / "broken"));
    tree.put("broken/scripts/campaigns.kv3", "{ campaigns = [] }");
    CHECK_FALSE(assets::addonCampaign(tree.root / "broken"));
    tree.put("broken/scripts/campaigns.kv3", "");
    CHECK_FALSE(assets::addonCampaign(tree.root / "broken", "main"));
}
