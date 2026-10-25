#include "assets/addons.hpp"
#include "assets/vfs.hpp"
#include "core/json.hpp"
#include "core/strings.hpp"
#include "fixture.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <vpkpp/format/VPK.h>

using namespace apex;
using namespace apex::assets;
namespace fs = std::filesystem;
namespace {
constexpr std::string_view header = "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n";
std::string entry(const fs::path& dir, bool mounted = true, std::string vpks = "[]") {
    return "{ dir = " + json::write({dir.string()}) + " mounted = " +
           (mounted ? "true" : "false") + " type = \"local\" vpks = " + vpks + " }";
}
void list(GameTree& tree, bool hammer, std::string entries) {
    tree.put(hammer ? "hammer/addons/addonlist.kv3" : "Game/addons/addonlist.kv3",
             std::string(header) + "{ addons = [ " + entries + " ] }");
}
GameProfile setup(GameTree& tree) {
    tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game Game/custom/* game |gameinfo_path|. } } }");
    tree.put("Game/materials/probe.vmt", "mod");
    tree.put("Game/materials/base.vmt", "base");
    tree.put("Game/custom/Pack/materials/probe.vmt", "custom");
    for (const auto* name : {"FirstAddon", "SecondAddon"}) {
        tree.put(fs::path(name) / "addon.kv3", std::string(header) + "{ mod = \"Apex fixture\" }");
        tree.put(fs::path(name) / "materials/probe.vmt", name);
    }
    GameProfile profile;
    profile.gameDirectory = tree.root / "Game";
    return profile;
}
std::string read(const Vfs& vfs, std::string_view path = "materials/probe.vmt") {
    auto bytes = vfs.open(path);
    REQUIRE(bytes);
    return {reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}
void pak(GameTree& tree, const fs::path& path, std::string_view text) {
    auto archive = vpkpp::VPK::create((tree.root / path).string(), 1);
    REQUIRE(archive);
    auto bytes = reinterpret_cast<const std::byte*>(text.data());
    REQUIRE(archive->addEntry("materials/probe.vmt", std::span(bytes, text.size())));
    REQUIRE(archive->bake());
}
}

TEST_CASE("addons resolve Wine paths component by component without losing missing entries") {
    GameTree tree;
    auto profile = setup(tree);
    profile.addons.list = AddonSettings::List::Hammer;  // Wine paths come from Hammer's list.
    CHECK(resolveAddonPath((tree.root / "FirstAddon").string() + "/") == tree.root / "FirstAddon");
    auto path = toLowerAscii((tree.root / "FirstAddon").string());
#ifndef _WIN32
    std::ranges::replace(path, '/', '\\');
    path = "z:" + path;
    CHECK(resolveAddonPath(path) == tree.root / "FirstAddon");
    path[0] = 'Z';
    CHECK(resolveAddonPath(path) == tree.root / "FirstAddon");
    CHECK(resolveAddonPath("C:\\Missing\\Addon").empty());
#else
    CHECK(resolveAddonPath(path) == tree.root / "FirstAddon");
    CHECK_FALSE(resolveAddonPath("C:\\").empty());
    // The fixture's native drive (C: on standard Windows builds) is retained.
    CHECK_FALSE(resolveAddonPath((tree.root / "FirstAddon").string()).empty());
#endif
    list(tree, true, entry(fs::path(path)) + entry("C:\\Missing\\Addon"));
    auto addons = readAddonList(profile);
    REQUIRE(addons);
    REQUIRE(addons->entries.size() == 2);
    CHECK(addons->entries[0].dir == path);
    CHECK(addons->entries[0].resolved == tree.root / "FirstAddon");
    CHECK(addons->entries[0].name == "Apex fixture");
    CHECK(addons->entries[1].resolved.empty());
    REQUIRE(addons->problems.size() == 1);
    CHECK(addons->problems[0].find("C:\\Missing\\Addon") != std::string::npos);
    fs::remove(tree.root / "FirstAddon/addon.kv3");
    addons = readAddonList(profile);
    REQUIRE(addons);
    CHECK(addons->entries[0].name == "FirstAddon");
    CHECK(addons->problems.size() == 2);
    tree.put("FirstAddon/addon.kv3", "broken");
    CHECK(readAddonList(profile)->problems.size() == 2);
}

TEST_CASE("addons first mounted entry wins above custom mod and head apps with provenance") {
    GameTree tree;
    auto profile = setup(tree);
    tree.put("Head/base/Content/materials/probe.vmt", "head");
    tree.put("Game/cfg/mounts.kv", "Mounts { 123 { head 1 base { dir Content } } }");
    JobPool jobs(2);
    auto load = [&] { return Vfs::load(profile, jobs, [&](std::uint32_t) { return tree.root / "Head"; }); };
    for (bool hammer : {false, true}) {
        profile.addons.list = hammer ? AddonSettings::List::Hammer : AddonSettings::List::Game;
        list(tree, hammer, entry(tree.root / "FirstAddon") + entry(tree.root / "SecondAddon"));
        auto vfs = load();
        REQUIRE(vfs);
        CHECK(read(*vfs) == "FirstAddon");
        CHECK(vfs->provider("MATERIALS\\PROBE.VMT") == 0);
        CHECK_FALSE(vfs->provider("materials/base.vmt"));
        CHECK_FALSE(vfs->provider("missing"));
        list(tree, hammer, entry(tree.root / "SecondAddon") + entry(tree.root / "FirstAddon"));
        vfs = load();
        REQUIRE(vfs);
        CHECK(read(*vfs) == "SecondAddon");
        list(tree, hammer, entry(tree.root / "SecondAddon", false) + entry(tree.root / "FirstAddon"));
        vfs = load();
        REQUIRE(vfs);
        CHECK(read(*vfs) == "FirstAddon");
        CHECK(vfs->provider("materials/probe.vmt") == 1);
    }
    profile.addons.enabled = false;
    auto vfs = load();
    REQUIRE(vfs);
    CHECK(read(*vfs) == "head");
    CHECK_FALSE(vfs->provider("materials/probe.vmt"));
    CHECK(vfs->addons().entries.empty());
}

TEST_CASE("addons VPK and loose priority follows source and assets stay excluded after refresh") {
    GameTree tree;
    auto profile = setup(tree);
    pak(tree, "FirstAddon/pak01_dir.vpk", "vpk first");
    pak(tree, "FirstAddon/pak02_dir.vpk", "vpk second");
    tree.put("FirstAddon/materials/loose.vmt", "always loose");
    tree.put("FirstAddon/.assets/thumb.txt", "UI art");
    tree.put("FirstAddon/.ASSETS/nested/art.txt", "UI art");
    const auto entries = entry(tree.root / "FirstAddon", true, "[ \"pak01_dir.vpk\", \"pak02_dir.vpk\", ]");
    list(tree, false, entries);
    list(tree, true, entries);
    JobPool jobs(2);
    for (auto source : {AddonSettings::List::Hammer, AddonSettings::List::Game, AddonSettings::List::Apex}) {
        profile.addons.list = source;
        profile.addons.apexMounted = {tree.root / "FirstAddon"};
        auto vfs = Vfs::load(profile, jobs);
        REQUIRE(vfs);
        CHECK(read(*vfs) == (source == AddonSettings::List::Hammer ? "FirstAddon" : "vpk first"));
        CHECK(read(*vfs, "materials/loose.vmt") == "always loose");
        CHECK(vfs->provider("materials/probe.vmt") == 0);
        for (int refresh = 0; refresh < 2; ++refresh) {
            CHECK_FALSE(vfs->exists(".assets/thumb.txt"));
            CHECK_FALSE(vfs->open(".assets/thumb.txt"));
            CHECK(vfs->list(".assets").empty());
            CHECK(vfs->walk(".assets").empty());
            auto all = vfs->walk("");
            CHECK(std::ranges::none_of(all, [](const auto& path) { return path.starts_with(".assets/"); }));
            tree.put("FirstAddon/.assets/new.txt", "new UI art");
            REQUIRE(vfs->refresh(jobs));
        }
    }
}

TEST_CASE("addons list fallback apex disabled corrupt lists and missing folders") {
    GameTree tree;
    auto profile = setup(tree);
    JobPool jobs(2);
    profile.addons.list = AddonSettings::List::Hammer;
    auto addons = readAddonList(profile);
    REQUIRE(addons);
    CHECK(addons->entries.empty());
    CHECK(addons->file.empty());
    list(tree, false, entry(tree.root / "SecondAddon"));
    addons = readAddonList(profile);
    REQUIRE(addons);
    CHECK(addons->source == AddonSettings::List::Game);
    CHECK(addons->file == tree.root / "Game/addons/addonlist.kv3");
    list(tree, true, entry(tree.root / "FirstAddon"));
    profile.gameDirectory /= "";
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    CHECK(read(*vfs) == "FirstAddon");
    CHECK(vfs->addons().source == AddonSettings::List::Hammer);
    profile.addons.list = AddonSettings::List::Game;
    vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    CHECK(read(*vfs) == "SecondAddon");
    CHECK(Vfs::load(GameProfile{.gameDirectory = profile.gameDirectory}, jobs)->addons().source ==
          AddonSettings::List::Game);  // The default ignores Hammer's list.
    profile.addons.list = AddonSettings::List::Apex;
    profile.addons.apexMounted = {tree.root / "SecondAddon", tree.root / "FirstAddon"};
    vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    CHECK(read(*vfs) == "SecondAddon");
    CHECK(vfs->addons().file.empty());
    profile.addons.list = AddonSettings::List::Hammer;
    tree.put("hammer/addons/addonlist.kv3", "broken list");
    CHECK_FALSE(readAddonList(profile));
    vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    CHECK(read(*vfs) == "custom");
    CHECK(vfs->addons().entries.empty());
    CHECK_FALSE(vfs->addons().problems.empty());
    list(tree, true, entry(tree.root / "missing") + entry(tree.root / "FirstAddon"));
    vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    REQUIRE(vfs->addons().entries.size() == 2);
    CHECK(vfs->addons().entries[0].resolved.empty());
    CHECK_FALSE(vfs->addons().problems.empty());
    CHECK(vfs->provider("materials/probe.vmt") == 1);
    profile.addons.enabled = false;
    vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    CHECK(read(*vfs) == "custom");
    CHECK(vfs->addons().entries.empty());
}

TEST_CASE("addons mounted write-back preserves bytes permissions and no-op mtime") {
    GameTree tree;
    auto profile = setup(tree);
    const auto file = tree.root / "Game/addons/addonlist.kv3";
    const auto original = std::string("\xef\xbb\xbf") +
        "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\r\n"
        "{ // Apex-authored fixture\r\n\taddons  = [\r\n" +
        entry(tree.root / "FirstAddon") + ", /* keep this */\r\n\t" +
        "{ dir = " + json::write({(tree.root / "SecondAddon").string()}) +
        " mounted\t=  /* flag */ false  type = \"local\" vpks = [] },\r\n] }\r\n";
    tree.put("Game/addons/addonlist.kv3", original);
    auto bytes = [&] {
        std::ifstream input(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{input}, {});
    };
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    const auto permissions = fs::status(file).permissions();
    const auto timestamp = fs::file_time_type::clock::now() - std::chrono::hours(24);
    fs::last_write_time(file, timestamp);
    const auto mtime = fs::last_write_time(file);
    auto addons = readAddonList(profile);
    REQUIRE(addons);
    REQUIRE(setAddonMounted(*addons, 1, false));
    CHECK(bytes() == original);
    CHECK(fs::last_write_time(file) == mtime);
    REQUIRE(setAddonMounted(*addons, 1, true));
    auto expected = original;
    expected.replace(expected.find("false"), 5, "true");
    CHECK(bytes() == expected);
    CHECK(fs::status(file).permissions() == permissions);
    REQUIRE(setAddonMounted(*addons, 1, false));
    CHECK(bytes() == original);
}

TEST_CASE("addons mounted write-back rejects stale and non-file lists without writes") {
    GameTree tree;
    auto profile = setup(tree);
    list(tree, false, entry(tree.root / "FirstAddon"));
    auto addons = readAddonList(profile);
    REQUIRE(addons);
    const auto file = addons->file;
    auto bytes = [&] {
        std::ifstream input(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{input}, {});
    };
    const auto original = bytes();
    auto result = setAddonMounted(*addons, 1, false);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::InvalidArgument);
    CHECK(bytes() == original);
    list(tree, false, entry(tree.root / "SecondAddon"));
    const auto changed = bytes();
    result = setAddonMounted(*addons, 0, false);
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "Addon list changed on disk; reload and try again");
    CHECK(bytes() == changed);
    list(tree, false, "");
    const auto empty = bytes();
    CHECK_FALSE(setAddonMounted(*addons, 0, false));
    CHECK(bytes() == empty);
    profile.addons.list = AddonSettings::List::Apex;
    profile.addons.apexMounted = {tree.root / "FirstAddon"};
    addons = readAddonList(profile);
    REQUIRE(addons);
    result = setAddonMounted(*addons, 0, false);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::InvalidArgument);
    CHECK(bytes() == empty);
    addons->source = AddonSettings::List::Game;
    CHECK_FALSE(setAddonMounted(*addons, 0, false));  // No file.
}

TEST_CASE("addons mounted write-back changes VFS provider after reload") {
    GameTree tree;
    auto profile = setup(tree);
    JobPool jobs(2);
    for (bool hammer : {false, true}) {
        profile.addons.list = hammer ? AddonSettings::List::Hammer : AddonSettings::List::Game;
        list(tree, hammer, entry(tree.root / "FirstAddon") + entry(tree.root / "SecondAddon"));
        auto addons = readAddonList(profile);
        REQUIRE(addons);
        auto vfs = Vfs::load(profile, jobs);
        REQUIRE(vfs);
        CHECK(vfs->provider("materials/probe.vmt") == 0);
        REQUIRE(setAddonMounted(*addons, 0, false));
        vfs = Vfs::load(profile, jobs);
        REQUIRE(vfs);
        CHECK(vfs->provider("materials/probe.vmt") == 1);
        CHECK(read(*vfs) == "SecondAddon");
        REQUIRE(setAddonMounted(*addons, 0, true));
        vfs = Vfs::load(profile, jobs);
        REQUIRE(vfs);
        CHECK(vfs->provider("materials/probe.vmt") == 0);
    }
}
