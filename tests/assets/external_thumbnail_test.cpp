#include "assets/thumbnail_cache.hpp"
#include "fixture.hpp"
#include "texture_fixture.hpp"

#include <doctest/doctest.h>
#include <thread>

using namespace apex;
using namespace apex::assets;

TEST_CASE("ThumbnailCache external model images share material LRU, budget and change notification") {
    GameTree tree;
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    REQUIRE(whiteVtf().bake(tree.root / "materials/white.vtf"));
    MaterialIndexEntry entry; entry.name = "same.mdl"; entry.previewTexture = "white";
    auto index = std::make_shared<MaterialIndexSnapshot>(std::vector{entry});
    JobPool jobs(2);
    GameProfile profile; profile.gameDirectory = tree.root;
    auto vfs = Vfs::load(profile, jobs); REQUIRE(vfs);
    ThumbnailCache cache(*vfs, jobs, index, 512); // Two 8px RGBA images, across both kinds.
    constexpr auto kind = ThumbnailKind::Model;
    const auto image = [] { return Thumbnail{8, 8, std::vector<std::uint8_t>(256, 123)}; };
    CHECK_FALSE(cache.request("same.mdl", 8));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cache.state("same.mdl", 8) == ThumbnailState::Loading && std::chrono::steady_clock::now() < deadline) {
        jobs.drainCompletions();
        std::this_thread::yield();
    }
    REQUIRE(cache.state("same.mdl", 8) == ThumbnailState::Ready);
    CHECK(cache.takeChanged());
    cache.insert(kind, "same", 7, image());
    CHECK(cache.takeChanged());
    CHECK_FALSE(cache.takeChanged());
    auto held = cache.request(kind, "SAME.MDL", 8); REQUIRE(held);
    CHECK(held->rgba[0] == 123);
    REQUIRE(cache.request("same.mdl", 8));
    CHECK(cache.request("same.mdl", 8)->rgba[0] == 255);
    CHECK(cache.residentBytes() == 512);
    cache.insert(kind, "other", 8, image());
    CHECK(cache.state(kind, "same", 8) == ThumbnailState::Unloaded); // Material touched last.
    CHECK(cache.state("same.mdl", 8) == ThumbnailState::Ready);
    CHECK(cache.residentBytes() == 512);
    cache.request(kind, "other", 8);
    cache.insert(kind, "same", 8, image());
    CHECK(cache.state("same.mdl", 8) == ThumbnailState::Unloaded); // Same shared LRU.
    cache.insert(kind, "same", 8, image()); // Replace without double-counting.
    CHECK(cache.residentBytes() == 512);
    cache.insert(kind, "bad", 8, std::unexpected(Error{ErrorCode::ParseError, "empty mesh"}));
    CHECK(cache.state(kind, "bad", 8) == ThumbnailState::Failed);
    REQUIRE(cache.error(kind, "bad", 8));
    CHECK(cache.takeChanged());
    cache.insert(kind, "invalid", 8, Thumbnail{8, 8, {1, 2, 3}});
    CHECK(cache.state(kind, "invalid", 8) == ThumbnailState::Failed);
    cache.setBudget(100);
    CHECK(cache.residentBytes() == 0);
    CHECK(held->rgba.size() == 256);
    cache.insert(kind, "oversized", 8, image());
    CHECK(cache.state(kind, "oversized", 8) == ThumbnailState::Failed);
    CHECK(cache.residentBytes() == 0);
    cache.setBudget(512);
    CHECK(cache.state(kind, "oversized", 8) == ThumbnailState::Unloaded);
    cache.insert(kind, "oversized", 8, image());
    CHECK(cache.state(kind, "oversized", 8) == ThumbnailState::Ready);
    CHECK(cache.state(kind, "bad", 8) == ThumbnailState::Failed);
}
