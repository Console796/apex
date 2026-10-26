#include "assets/thumbnail_cache.hpp"
#include "fixture.hpp"
#include "texture_fixture.hpp"

#include <doctest/doctest.h>
#include <thread>

using namespace apex;
using namespace apex::assets;
using vtfpp::ImageFormat;
namespace {
void finish(JobPool& jobs, ThumbnailCache& cache, std::string_view name, int size) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cache.state(name, size) == ThumbnailState::Loading && std::chrono::steady_clock::now() < deadline) {
        jobs.drainCompletions();
        std::this_thread::yield();
    }
    REQUIRE(cache.state(name, size) != ThumbnailState::Loading);
}
}
TEST_CASE("thumbnail decoder covers fixture formats and selects smallest sufficient mip, then box-filters") {
    std::vector<ImageFormat> formats;
    for (int i = 0; i <= 38; ++i)
        if (i != int(ImageFormat::EMPTY)) formats.push_back(static_cast<ImageFormat>(i));
    for (int i : {66, 67, 69, 70, 71, 72, 73, 74}) formats.push_back(static_cast<ImageFormat>(i));
    for (auto format : formats) {
        CAPTURE(int(format));
        auto vtf = whiteVtf(format);
        auto baked = vtf.bake();
        for (int size : {1, 3, 4, 8, 32}) {
            auto thumb = decodeThumbnail(baked, size);
            REQUIRE_MESSAGE(thumb, (thumb ? "" : thumb.error().what()));
            std::uint8_t expected = 0;
            while (expected + 1 < vtf.getMipCount() &&
                   std::max(vtf.getWidth(static_cast<std::uint8_t>(expected + 1)), vtf.getHeight(static_cast<std::uint8_t>(expected + 1))) >= size) ++expected;
            const int mipWidth = vtf.getWidth(expected), mipHeight = vtf.getHeight(expected);
            int factor = 1; // Mips more than twice the size are box-filtered down.
            while (std::max(mipWidth, mipHeight) / (factor * 2) >= size) factor *= 2;
            CHECK(thumb->width == std::max(1, mipWidth / factor));
            CHECK(thumb->height == std::max(1, mipHeight / factor));
            CHECK(thumb->rgba.size() == std::size_t(thumb->width) * thumb->height * 4);
            std::array<int, 4> white{255, 255, 255, 255};
            if (format == ImageFormat::UV88 || format == ImageFormat::ATI2N ||
                format == ImageFormat::STRATA_BC5 || format == ImageFormat::RG1616F ||
                format == ImageFormat::RG3232F) white[2] = 0;
            if (format == ImageFormat::STRATA_R8 || format == ImageFormat::R16F || format == ImageFormat::R32F)
                white[1] = white[2] = 0;
            for (std::size_t i = 0; i < thumb->rgba.size(); ++i)
                CHECK(std::abs(int(thumb->rgba[i]) - white[i % 4]) <= 3);
        }
    }
    CHECK_FALSE(decodeThumbnail({}, 64));
    auto bytes = whiteVtf().bake();
    CHECK_FALSE(decodeThumbnail(bytes, 0));
    bytes.resize(bytes.size() - 20);
    CHECK_FALSE(decodeThumbnail(bytes, 8));
}

TEST_CASE("thumbnail uses frame face slice zero and clamps HDR pixels") {
    auto vtf = whiteVtf();
    REQUIRE(vtf.setFrameFaceAndDepth(2, true, 2));
    for (std::uint8_t mip = 0; mip < vtf.getMipCount(); ++mip) {
        auto width = vtf.getWidth(mip), height = vtf.getHeight(mip);
        for (std::uint16_t frame = 0; frame < 2; ++frame)
            for (std::uint8_t face = 0; face < vtf.getFaceCount(); ++face)
                for (std::uint16_t slice = 0; slice < vtf.getDepth(mip); ++slice) {
                    auto value = std::byte(20 + mip * 10 + frame * 30 + face * 3 + slice);
                    std::vector<std::byte> pixels(std::size_t(width) * height * 4, value);
                    REQUIRE(vtf.setImage(pixels, ImageFormat::RGBA8888, width, height,
                        vtfpp::ImageConversion::ResizeFilter::DEFAULT, mip, frame, face, slice));
                }
    }
    auto thumb = decodeThumbnail(vtf.bake(), 3);
    REQUIRE(thumb);
    CHECK(thumb->width == 4);
    for (auto pixel : thumb->rgba) CHECK(pixel == 30);

    std::vector<float> hdr(8 * 8 * 4);
    for (std::size_t i = 0; i < hdr.size(); i += 4) {
        hdr[i] = -1; hdr[i + 1] = 0.5f; hdr[i + 2] = 4; hdr[i + 3] = 1;
    }
    vtfpp::VTF::CreationOptions options;
    options.outputFormat = ImageFormat::RGBA32323232F;
    options.computeMips = false;
    options.computeThumbnail = false;
    options.computeReflectivity = false;
    auto image = vtfpp::VTF::create(std::as_bytes(std::span(hdr)), ImageFormat::RGBA32323232F, 8, 8, options);
    auto clamped = decodeThumbnail(image.bake(), 8);
    REQUIRE(clamped);
    CHECK(clamped->rgba[0] == 0);
    CHECK(clamped->rgba[1] == 127);
    CHECK(clamped->rgba[2] == 255);
    CHECK(clamped->rgba[3] == 255);
}

TEST_CASE("ThumbnailCache scrolls 20000 indexed materials within budget and reloads evictions") {
    GameTree tree;
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("materials/.keep", "");
    REQUIRE(whiteVtf().bake(tree.root / "materials/white.vtf"));
    tree.put("materials/bad.vtf", "broken");
    std::vector<MaterialIndexEntry> entries;
    for (int i = 0; i < 20000; ++i) {
        MaterialIndexEntry entry;
        entry.name = "material/" + std::to_string(i);
        entry.previewTexture = "white";
        entries.push_back(std::move(entry));
    }
    MaterialIndexEntry bad; bad.name = "bad"; bad.previewTexture = "bad";
    entries.push_back(bad);
    MaterialIndexEntry empty; empty.name = "empty";
    entries.push_back(empty);
    auto index = std::make_shared<MaterialIndexSnapshot>(std::move(entries));
    JobPool jobs(2);
    GameProfile profile; profile.gameDirectory = tree.root;
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    constexpr std::size_t budget = 256 * 64;
    ThumbnailCache cache(*vfs, jobs, index, budget);
    CHECK(ThumbnailCache::sizeBucket(5) == 8);
    CHECK(ThumbnailCache::sizeBucket(7) == 8);
    CHECK_FALSE(cache.takeChanged());
    for (int start = 0; start < 20000; start += 200) {
        for (int i = start; i < start + 200; ++i) {
            auto name = "material/" + std::to_string(i);
            CHECK_FALSE(cache.request(name, 8));
            CHECK_FALSE(cache.request(name, 7)); // same bucket, no second job
        }
        for (int i = start; i < start + 200; ++i) {
            finish(jobs, cache, "material/" + std::to_string(i), 8);
            REQUIRE(cache.residentBytes() <= budget);
        }
        CHECK(cache.takeChanged());
        CHECK_FALSE(cache.takeChanged());
    }
    CHECK(cache.state("material/0", 8) == ThumbnailState::Unloaded);
    CHECK_FALSE(cache.request("MATERIALS/MATERIAL/0.VMT", 8));
    finish(jobs, cache, "material/0", 8);
    auto held = cache.request("material/0", 8);
    REQUIRE(held);
    CHECK_FALSE(cache.request("material/0", 4)); // distinct bucket
    finish(jobs, cache, "material/0", 4);
    CHECK(cache.request("material/0", 4)->width == 4);
    cache.setBudget(0);
    CHECK(cache.residentBytes() == 0);
    CHECK(held->width == 8);
    for (const auto* name : {"bad", "missing", "empty"}) {
        CHECK_FALSE(cache.request(name, 8));
        finish(jobs, cache, name, 8);
        CHECK(cache.state(name, 8) == ThumbnailState::Failed);
        REQUIRE(cache.error(name, 8));
        CHECK_FALSE(cache.request(name, 8));
        CHECK(cache.state(name, 8) == ThumbnailState::Failed);
    }
    { ThumbnailCache temporary(*vfs, jobs, index); temporary.request("material/0", 8); }
    jobs.drainCompletions();
}

TEST_CASE("thumbnail box-filters a large texture without small mips") {
    auto vtf = whiteVtf();
    REQUIRE(vtf.setMipCount(1));
    auto thumb = decodeThumbnail(vtf.bake(), 2);
    REQUIRE(thumb);
    CHECK(thumb->width == 2);
    CHECK(thumb->height == 2);
    for (auto value : thumb->rgba) CHECK(value == 255);
}

TEST_CASE("ThumbnailCache keeps a thumbnail larger than its budget failed until the budget changes") {
    GameTree tree;
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    REQUIRE(whiteVtf().bake(tree.root / "materials/white.vtf"));
    MaterialIndexEntry entry; entry.name = "white"; entry.previewTexture = "white";
    auto index = std::make_shared<MaterialIndexSnapshot>(std::vector{entry});
    JobPool jobs(2);
    GameProfile profile; profile.gameDirectory = tree.root;
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    ThumbnailCache cache(*vfs, jobs, index, 100); // An 8x8 RGBA thumbnail is 256 bytes.
    CHECK_FALSE(cache.request("white", 8));
    finish(jobs, cache, "white", 8);
    CHECK(cache.state("white", 8) == ThumbnailState::Failed);
    CHECK(cache.takeChanged());
    CHECK_FALSE(cache.request("white", 8)); // No reload loop: still failed, no new job.
    CHECK(cache.state("white", 8) == ThumbnailState::Failed);
    CHECK(cache.residentBytes() == 0);
    cache.setBudget(1024);
    CHECK(cache.state("white", 8) == ThumbnailState::Unloaded);
    CHECK_FALSE(cache.request("white", 8));
    finish(jobs, cache, "white", 8);
    CHECK(cache.request("white", 8));
}
