#include "assets/vfs.hpp"
#include "fixture.hpp"

#include <algorithm>
#include <cstdlib>
#include <doctest/doctest.h>
#include <vpkpp/format/VPK.h>

using namespace apex;
using namespace apex::assets;
namespace fs = std::filesystem;

namespace {
void pak(GameTree& tree, const fs::path& name, std::string_view entry, std::string_view contents) {
    fs::create_directories((tree.root / name).parent_path());
    auto archive = vpkpp::VPK::create((tree.root / name).string(), 1);
    REQUIRE(archive);
    const auto* bytes = reinterpret_cast<const std::byte*>(contents.data());
    REQUIRE(archive->addEntry(std::string(entry), std::span(bytes, contents.size())));
    REQUIRE(archive->bake());
}
std::string read(const Vfs& vfs, std::string_view path) {
    auto bytes = vfs.open(path);
    REQUIRE(bytes);
    return {reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}
GameProfile profile(const GameTree& tree) {
    GameProfile result;
    result.name = "fixture";
    result.gameDirectory = tree.root / "Game";
    return result;
}
} // namespace

TEST_CASE("VFS search forms are ordered case insensitive and refreshable") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", R"("GameInfo" { "FileSystem" { "SearchPaths" {
        game "Game/Custom/*"
        game+mod+default_write_path "|gameinfo_path|."
        GAME "|all_source_engine_paths|other"
        game "Vpks/*.vpk"
        game "direct/BASE.vpk"
        game "Explicit/pack_dir.vpk"
        game "Single/standalone.vpk"
        game "Inactive" [!$POSIX]
        mod "ModOnly"
        game "missing"
        gamebin "Bin"
    } } })");
    tree.put("Game/Custom/A/Materials/Override.VMT", "custom");
    tree.put("Game/Materials/Override.vmt", "game");
    tree.put("Game/Materials/Loose.VMT", "loose");
    tree.put("Other/Materials/Other.vmt", "engine");
    tree.put("Bin/Materials/hidden.vmt", "wrong path id");
    tree.put("ModOnly/Materials/modonly.vmt", "wrong path id");
    tree.put("Game/empty.txt", "");
    pak(tree, "Game/Custom/B_dir.vpk", "materials/custompak.vmt", "custompak");
    pak(tree, "Vpks/a_dir.vpk", "materials/wildcard.vmt", "wildcard");
    pak(tree, "Vpks/b_dir.vpk", "materials/override.vmt", "archive loses");
    pak(tree, "Direct/base_dir.vpk", "Materials/Direct.vmt", "direct");
    pak(tree, "Explicit/pack_dir.vpk", "materials/explicit.vmt", "explicit");
    tree.put("Direct/base.vpk", "invalid base; use _dir fallback");
    pak(tree, "Single/standalone.vpk", "materials/single.vmt", "single");
    JobPool jobs(2);
    auto vfs = Vfs::load(profile(tree), jobs);
    REQUIRE(vfs);
    CHECK(read(*vfs, "MATERIALS\\OVERRIDE.vMt") == "custom");
    CHECK(read(*vfs, "materials/LOOSE.vmt") == "loose");
    CHECK(read(*vfs, "materials/other.vmt") == "engine");
    CHECK(read(*vfs, "MATERIALS/DIRECT.VMT") == "direct");
    CHECK(read(*vfs, "materials/explicit.vmt") == "explicit");
    CHECK(read(*vfs, "materials/wildcard.vmt") == "wildcard");
    CHECK(read(*vfs, "materials/custompak.vmt") == "custompak");
    CHECK(read(*vfs, "materials/single.vmt") == "single");
    CHECK(read(*vfs, "empty.txt").empty());
    CHECK_FALSE(vfs->exists("materials/modonly.vmt"));
    CHECK_FALSE(vfs->exists("materials/hidden.vmt"));
    CHECK_FALSE(vfs->exists("../materials/loose.vmt"));
    CHECK_FALSE(vfs->open("/materials/loose.vmt"));
    CHECK_FALSE(vfs->open("missing"));
    CHECK(vfs->list("MATERIALS", "*ERRIDE.V?T") == std::vector<std::string>{"materials/override.vmt"});
    CHECK(vfs->list("materials", "*.vmt").size() == 8);
    tree.put("Game/Materials/New.VMT", "new");
    fs::remove(tree.root / "Game/Materials/Loose.VMT");
    CHECK_FALSE(vfs->exists("materials/new.vmt"));
    REQUIRE(vfs->refresh(jobs));
    CHECK(read(*vfs, "materials/new.vmt") == "new");
    CHECK_FALSE(vfs->exists("materials/loose.vmt"));
}

TEST_CASE("VFS Steam mounts honor enabled required head mod folders VPK and dir") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", R"(GameInfo { FileSystem { SearchPaths { game "|gameinfo_path|." } } })");
    tree.put("Game/materials/priority.vmt", "game");
    tree.put("Game/cfg/mounts.kv", R"(Mounts {
        123 { enabled 1 required 1 head 1 mod_folder { vpk "pak" dir "Download" } }
        124 { enabled 1 head 1 mod_folder { dir "Download" } }
        125 { enabled 1 mod_folder { dir "Download" } }
        999 { enabled 0 required 1 mod_folder { vpk "missing" } }
        888 { enabled 1 required 0 mod_folder { vpk "missing" } }
    })");
    pak(tree, "SteamLibrary/steamapps/common/Fake/mod_folder/pak_dir.vpk", "materials/mounted.vmt", "steam pak");
    tree.put("SteamLibrary/steamapps/common/Fake/mod_folder/Download/Materials/Priority.VMT", "head");
    tree.put("SteamLibrary/steamapps/common/Fake/mod_folder/Download/Materials/download.vmt", "download");
    tree.put("Second/mod_folder/Download/materials/priority.vmt", "second head");
    tree.put("Tail/mod_folder/Download/materials/priority.vmt", "tail");
    JobPool jobs(1);
    auto resolver = [&](std::uint32_t id) -> fs::path {
        if (id == 123) return tree.root / "SteamLibrary/steamapps/common/Fake";
        if (id == 124) return tree.root / "Second";
        if (id == 125) return tree.root / "Tail";
        return {};
    };
    auto vfs = Vfs::load(profile(tree), jobs, resolver);
    REQUIRE(vfs);
    CHECK(read(*vfs, "materials/MOUNTED.vmt") == "steam pak");
    CHECK(read(*vfs, "materials/priority.vmt") == "head");
    CHECK(read(*vfs, "materials/download.vmt") == "download");
    tree.put("Game/cfg/mounts.kv", R"(Mounts { 999 { enabled 1 required 1 mod_folder { vpk missing } } })");
    CHECK_FALSE(Vfs::load(profile(tree), jobs, resolver));
    tree.put("Game/cfg/mounts.kv", R"(Mounts { 123 { required 1 mod_folder { vpk missing } } })");
    CHECK_FALSE(Vfs::load(profile(tree), jobs, resolver));
}

TEST_CASE("VFS archive and loose precedence follow search path order") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", R"(GameInfo { FileSystem { SearchPaths {
        game "Archive/first.vpk" game "|gameinfo_path|."
    } } })");
    tree.put("Game/materials/shared.vmt", "loose");
    pak(tree, "Archive/first_dir.vpk", "materials/shared.vmt", "archive");
    JobPool jobs(1);
    auto vfs = Vfs::load(profile(tree), jobs);
    REQUIRE(vfs);
    CHECK(read(*vfs, "materials/shared.vmt") == "archive");
    CHECK(vfs->list("materials").size() == 1);
    tree.put("Game/gameinfo.txt", R"(GameInfo { FileSystem { SearchPaths {
        game "|gameinfo_path|." game "Archive/first.vpk"
    } } })");
    auto reversed = Vfs::load(profile(tree), jobs);
    REQUIRE(reversed);
    CHECK(read(*reversed, "materials/shared.vmt") == "loose");
    fs::remove_all(tree.root / "Game");
    CHECK_FALSE(reversed->refresh(jobs));
    CHECK(reversed->exists("materials/shared.vmt")); // Failed refresh keeps the old index.
    CHECK_FALSE(reversed->open("materials/shared.vmt")); // A failed read is not a fallback.
}

TEST_CASE("VFS rejects missing and malformed game configuration") {
    GameTree tree;
    JobPool jobs(1);
    CHECK_FALSE(Vfs::load(profile(tree), jobs));
    tree.put("Game/gameinfo.txt", "GameInfo {}");
    CHECK_FALSE(Vfs::load(profile(tree), jobs));
    tree.put("Game/gameinfo.txt", "GameInfo {");
    CHECK_FALSE(Vfs::load(profile(tree), jobs));
}

TEST_CASE("VFS resolves real P2CE loose and Portal 2 VPK material") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    GameProfile game;
    game.name = "P2CE integration";
    game.gameDirectory = fs::path(install) / "p2ce";
    JobPool jobs(2);
    auto vfs = Vfs::load(game, jobs);
    REQUIRE(vfs);
    CHECK_FALSE(read(*vfs, "MATERIALS/TOOLS/TOOLSNODRAW.VMT").empty());
    constexpr auto material = "materials/models/props/saucepan/saucepan.vmt";
    CHECK_FALSE(fs::exists(fs::path(install) / "portal2/portal2" / material));
    CHECK_FALSE(fs::exists(fs::path(install) / "p2ce" / material));
    CHECK_FALSE(read(*vfs, material).empty());
}

TEST_CASE("P0 red: P2CE material enumeration includes subdirectories (D14)") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    GameProfile game;
    game.name = "P2CE integration";
    game.gameDirectory = fs::path(install) / "p2ce";
    JobPool jobs(2);
    auto vfs = Vfs::load(game, jobs);
    REQUIRE(vfs);
    REQUIRE(vfs->exists("materials/tools/toolsnodraw.vmt"));
    const auto materials = vfs->list("materials", "*.vmt");
    CHECK_MESSAGE(std::ranges::any_of(materials, [](const auto& path) {
        return path.starts_with("materials/") && path.find('/', std::string_view("materials/").size()) != std::string::npos;
    }), "D14: material enumeration must include VMTs in subdirectories");
}
