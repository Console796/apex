#include "assets/vfs.hpp"
#include "fixture.hpp"

#include <doctest/doctest.h>
#include <future>
#include <vpkpp/format/VPK.h>

using namespace apex;
using namespace apex::assets;

TEST_CASE("VFS walk recurses across loose and VPK mounts with canonical paths and precedence") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", R"(GameInfo { FileSystem { SearchPaths {
        game "First/pak.vpk" game "|gameinfo_path|." game "Last"
    } } })");
    tree.put("Game/Materials/Nested/Shared.VMT", "loose loses");
    tree.put("Game/Materials/Nested/Loose.VMT", "loose wins");
    tree.put("Game/Materials/root.vmt", "root");
    tree.put("Game/Materials/Nested/deeper/image.vtf", "texture");
    tree.put("Game/MaterialsExtra/ignored.vmt", "other directory");
    tree.put("Last/materials/nested/loose.vmt", "tail loses");
    tree.put("Last/materials/tail/only.vmt", "tail");
    std::filesystem::create_directories(tree.root / "First");
    auto archive = vpkpp::VPK::create((tree.root / "First/pak_dir.vpk").string(), 1);
    REQUIRE(archive);
    for (const auto entry : {"Materials/Nested/Shared.VMT", "materials/vpk/deeper/only.vmt"}) {
        constexpr std::string_view contents = "archive wins";
        REQUIRE(archive->addEntry(entry, std::span(
            reinterpret_cast<const std::byte*>(contents.data()), contents.size())));
    }
    REQUIRE(archive->bake());
    archive.reset();
    JobPool jobs(2);
    GameProfile profile;
    profile.gameDirectory = tree.root / "Game";
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    const std::vector<std::string> expected{
        "materials/nested/loose.vmt", "materials/nested/shared.vmt", "materials/root.vmt",
        "materials/tail/only.vmt", "materials/vpk/deeper/only.vmt"};
    CHECK(vfs->walk("MATERIALS\\./", "*.VmT") == expected);
    CHECK(vfs->list("materials", "*.vmt") == std::vector<std::string>{"materials/root.vmt"});
    CHECK(vfs->walk("materials/nested", "?hared.v?t") ==
          std::vector<std::string>{"materials/nested/shared.vmt"});
    CHECK(vfs->walk("materials/nested/deeper").size() == 1);
    CHECK(vfs->walk("missing").empty());
    CHECK(vfs->walk("../materials").empty());
    CHECK(vfs->walk("/materials").empty());
    CHECK(vfs->walk("C:/materials").empty());
    CHECK(vfs->walk("", "*.vmt").size() == expected.size() + 1);
    for (const auto& path : expected) {
        const auto bytes = vfs->open(path);
        REQUIRE(bytes);
        const std::string value(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        if (path == "materials/nested/shared.vmt") CHECK(value == "archive wins");
        if (path == "materials/nested/loose.vmt") CHECK(value == "loose wins");
    }
    // Concurrent walks read only immutable indices, including archive indices.
    const auto worker = [&] { return vfs->walk("materials", "*.vmt"); };
    auto first = std::async(std::launch::async, worker);
    auto second = std::async(std::launch::async, worker);
    CHECK(first.get() == expected);
    CHECK(second.get() == expected);
    tree.put("Game/materials/new/deep.vmt", "new");
    CHECK(vfs->walk("materials", "*.vmt") == expected);
    REQUIRE(vfs->refresh(jobs));
    CHECK(vfs->walk("materials", "*.vmt").size() == expected.size() + 1);
}
