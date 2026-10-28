#include "assets/material_cache.hpp"
#include "fixture.hpp"
#include "texture_fixture.hpp"

#include <doctest/doctest.h>
#include <thread>

using namespace apex;
using namespace apex::assets;
namespace {
void finish(JobPool& jobs, MaterialCache& cache, std::string_view name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cache.state(name) == MaterialState::Loading && std::chrono::steady_clock::now() < deadline) {
        jobs.drainCompletions();
        std::this_thread::yield();
    }
    REQUIRE(cache.state(name) != MaterialState::Loading);
}
}
TEST_CASE("MaterialCache async completion placeholder failures and decoded LRU") {
    GameTree tree;
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("materials/a.vmt", "UnlitGeneric { $basetexture white $basetexture2 white }");
    tree.put("materials/b.vmt", "UnlitGeneric { $basetexture white }");
    tree.put("materials/bad.vmt", "UnlitGeneric { $basetexture bad }");
    tree.put("materials/bad.vtf", "invalid");
    tree.put("materials/empty.vtf", "");
    tree.put("materials/secondary.vmt", "UnlitGeneric { $basetexture white $basetexture2 empty }");
    auto vtf = whiteVtf();
    REQUIRE(vtf.bake(tree.root / "materials/white.vtf"));
    JobPool jobs(1);
    GameProfile profile;
    profile.gameDirectory = tree.root;
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    MaterialCache cache(*vfs, jobs);
    CHECK(cache.state("a") == MaterialState::Unloaded);
    CHECK(cache.request("a") == cache.placeholder());
    CHECK(cache.state("a") == MaterialState::Loading);
    CHECK(cache.request("MATERIALS/A.VMT") == cache.placeholder());
    finish(jobs, cache, "a");
    REQUIRE(cache.state("a") == MaterialState::Ready);
    auto held = cache.request("a");
    REQUIRE(held->texture2);
    const auto single = held->texture.byteSize();
    CHECK(cache.residentBytes() == single * 2);
    cache.request("b");
    finish(jobs, cache, "b");
    cache.request("a"); // a is most recently used
    cache.setBudget(single * 2);
    CHECK(cache.state("b") == MaterialState::Unloaded);
    CHECK(cache.state("a") == MaterialState::Ready);
    cache.setBudget(0);
    CHECK(cache.residentBytes() == 0);
    CHECK(held->texture.byteSize() == single); // caller snapshot survives eviction
    CHECK(cache.request("missing") == cache.placeholder());
    CHECK(cache.state("missing") == MaterialState::Failed);
    REQUIRE(cache.error("missing"));
    cache.request("bad");
    finish(jobs, cache, "bad");
    CHECK(cache.state("bad") == MaterialState::Failed);
    CHECK(cache.error("bad").has_value());
    cache.request("secondary");
    finish(jobs, cache, "secondary");
    CHECK(cache.state("secondary") == MaterialState::Failed);
    { MaterialCache temporary(*vfs, jobs); temporary.request("a"); }
    // Completion after destruction must be harmless, including under ASan.
    jobs.drainCompletions();
}

TEST_CASE("MaterialCache preserves Windows texture paths through VMT patches") {
    GameTree tree;
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("materials/dev/normal_pbr.vmt", R"(PBR { $model 1 })");
    tree.put("materials/door.vmt", R"(patch {
        include "materials\dev\normal_pbr.vmt"
        insert { $basetexture "models\pbr\test_chambers\testchmb_door_Color" }
    })");
    std::filesystem::create_directories(tree.root / "materials/models/pbr/test_chambers");
    auto texture = whiteVtf();
    REQUIRE(texture.bake(tree.root / "materials/models/pbr/test_chambers/testchmb_door_color.vtf"));
    JobPool jobs(1);
    GameProfile profile;
    profile.gameDirectory = tree.root;
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    MaterialCache cache(*vfs, jobs);
    cache.request("door");
    finish(jobs, cache, "door");
    REQUIRE_MESSAGE(cache.state("door") == MaterialState::Ready,
                    (cache.error("door") ? cache.error("door")->what() : ""));
    CHECK(cache.request("door")->material.baseTexture == R"(models\pbr\test_chambers\testchmb_door_Color)");
}
