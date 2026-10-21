#include "assets/vtf.hpp"
#include "texture_fixture.hpp"

#include <cmath>
#include <cstring>
#include <doctest/doctest.h>

using namespace apex::assets;
using vtfpp::ImageFormat;

TEST_CASE("VTF writer fixtures cover PC pixel formats and GPU compressed mip chains") {
    std::vector<ImageFormat> formats;
    // All universal and Alien Swarm PC formats except EMPTY. P8 is supported by
    // vtfpp as luminance; palette resources are an Xbox-only extension.
    for (int i = 0; i <= 38; ++i)
        if (i != int(ImageFormat::EMPTY)) {
            formats.push_back(static_cast<ImageFormat>(i));
        }
    for (int i : {66, 67, 69, 70, 71, 72, 73, 74}) {
        formats.push_back(static_cast<ImageFormat>(i));
    }
    for (auto format : formats) {
        CAPTURE(static_cast<int>(format));
        auto vtf = whiteVtf(format);
        REQUIRE(vtf);
        REQUIRE(vtf.getFormat() == format);
        auto decoded = decodeTexture(vtf.bake());
        REQUIRE_MESSAGE(decoded, (decoded ? "" : decoded.error().what()));
        CHECK(decoded->reflectivity == std::array<float, 3>{0.125f, 0.25f, 0.5f});
        CHECK(decoded->mips.size() == vtf.getMipCount());
        CHECK(decoded->mips.front().width == 8);
        for (std::size_t i = 0; i < decoded->mips.size(); ++i) {
            auto mip = static_cast<std::uint8_t>(i);
            const auto& output = decoded->mips[i];
            CHECK(output.width == vtf.getWidth(mip));
            std::vector<std::byte> pixels;
            if (decoded->format != TextureFormat::Rgba8 && decoded->format != TextureFormat::Rgba32Float) {
                auto raw = vtf.getImageDataRaw(mip);
                CHECK(output.data == std::vector<std::byte>(raw.begin(), raw.end()));
                pixels = vtfpp::ImageConversion::convertImageDataToFormat(
                    output.data, format, ImageFormat::RGBA8888, output.width, output.height);
            } else if (decoded->format == TextureFormat::Rgba32Float) {
                pixels = vtfpp::ImageConversion::convertImageDataToFormat(
                    output.data, ImageFormat::RGBA32323232F, ImageFormat::RGBA8888,
                    output.width, output.height);
            } else pixels = output.data;
            REQUIRE(pixels.size() == std::size_t(output.width) * output.height * 4);
            // White survives quantization. Missing colour channels are zero, except
            // luminance/BC4 which replicate into RGB and alpha-only which uses white RGB.
            std::array<int, 4> expected{255, 255, 255, 255};
            if (format == ImageFormat::UV88 || format == ImageFormat::ATI2N ||
                format == ImageFormat::STRATA_BC5 || format == ImageFormat::RG1616F ||
                format == ImageFormat::RG3232F) {
                expected[2] = 0;
            }
            if (format == ImageFormat::STRATA_R8 || format == ImageFormat::R16F || format == ImageFormat::R32F) {
                expected[1] = expected[2] = 0;
            }
            for (std::size_t pixel = 0; pixel < pixels.size(); ++pixel) {
                CAPTURE(i);
                CAPTURE(pixel);
                CHECK(std::abs(int(pixels[pixel]) - expected[pixel % 4]) <= 3);
            }
        }
    }
}

TEST_CASE("VTF versions 7.0 through 7.6 and AXC Deflate Zstd preserve pixels") {
    for (std::uint32_t version = 0; version <= 6; ++version) {
        CAPTURE(version);
        auto vtf = whiteVtf(ImageFormat::RGBA8888, version);
        auto decoded = decodeTexture(vtf.bake());
        REQUIRE(decoded);
        CHECK(decoded->mips.front().data == std::vector<std::byte>(8 * 8 * 4, std::byte{255}));
    }
    for (auto format : {ImageFormat::RGBA8888, ImageFormat::DXT1, ImageFormat::STRATA_BC7,
                        ImageFormat::STRATA_BC6H_UF, ImageFormat::STRATA_BC6H_SF}) {
        for (auto method : {vtfpp::CompressionMethod::DEFLATE, vtfpp::CompressionMethod::ZSTD}) {
            CAPTURE(static_cast<int>(format));
            CAPTURE(static_cast<int>(method));
            auto vtf = whiteVtf(format);
            auto original = decodeTexture(vtf.bake());
            REQUIRE(original);
            vtf.setCompressionLevel(6);
            vtf.setCompressionMethod(method);
            auto baked = vtf.bake();
            vtfpp::VTF reread(baked);
            REQUIRE(reread.getResource(vtfpp::Resource::TYPE_IMAGE_DATA));
            CHECK(reread.getCompressionLevel() == 6);
            CHECK(reread.getCompressionMethod() == method);
            auto decoded = decodeTexture(baked);
            REQUIRE(decoded);
            REQUIRE(decoded->mips.size() == original->mips.size());
            for (std::size_t i = 0; i < decoded->mips.size(); ++i)
                CHECK(decoded->mips[i].data == original->mips[i].data);
        }
    }
}

TEST_CASE("VTF frame face and volume slice layout is complete") {
    auto vtf = whiteVtf();
    REQUIRE(vtf.setFrameFaceAndDepth(2, true));
    auto decoded = decodeTexture(vtf.bake());
    REQUIRE(decoded);
    CHECK(decoded->frames == 2);
    CHECK(decoded->faces == 6);
    CHECK(decoded->mips.front().data.size() == 8 * 8 * 4 * 2 * 6);
    auto volume = whiteVtf();
    REQUIRE(volume.setDepth(4));
    auto slices = decodeTexture(volume.bake());
    REQUIRE(slices);
    CHECK(slices->mips.front().depth == 4);
    CHECK(slices->mips.front().data.size() == 8 * 8 * 4 * 4);
}

TEST_CASE("VTF rejects invalid data and supplies an opaque checkerboard") {
    CHECK_FALSE(decodeTexture({}));
    std::vector<std::byte> junk(80, std::byte{0});
    CHECK_FALSE(decodeTexture(junk));
    auto truncated = whiteVtf(ImageFormat::RGBA8888, 2).bake();
    truncated.resize(truncated.size() - 1);
    CHECK_FALSE(decodeTexture(truncated));
    auto placeholder = checkerboardTexture();
    CHECK(placeholder.byteSize() == 8 * 8 * 4);
    CHECK(placeholder.mips[0].data[0] != placeholder.mips[0].data[4 * 4]);
    for (std::size_t i = 3; i < placeholder.byteSize(); i += 4)
        CHECK(placeholder.mips[0].data[i] == std::byte{255});
}

TEST_CASE("VTF pixel conversion preserves channel order and HDR range") {
    const std::array<std::byte, 4> rgba{std::byte{17}, std::byte{83}, std::byte{191}, std::byte{255}};
    for (auto format : {ImageFormat::RGBA8888, ImageFormat::ABGR8888, ImageFormat::ARGB8888,
                        ImageFormat::BGRA8888, ImageFormat::RGB888, ImageFormat::BGR888}) {
        CAPTURE(static_cast<int>(format));
        std::vector<std::byte> pixels;
        for (unsigned i = 0; i < 16; ++i) {
            pixels.insert(pixels.end(), rgba.begin(), rgba.end());
        }
        vtfpp::VTF::CreationOptions options;
        options.version = 6;
        options.outputFormat = format;
        options.computeMips = false;
        options.computeThumbnail = false;
        auto vtf = vtfpp::VTF::create(pixels, ImageFormat::RGBA8888, 4, 4, options);
        auto decoded = decodeTexture(vtf.bake());
        REQUIRE(decoded);
        CHECK(decoded->format == TextureFormat::Rgba8);
        CHECK(decoded->mips.front().data == pixels);
    }
    const std::array<float, 4> hdr{0.25f, 2.f, 4.f, 0.5f};
    std::vector<std::byte> pixels(16 * sizeof(hdr));
    for (unsigned i = 0; i < 16; ++i) {
        std::memcpy(pixels.data() + i * sizeof(hdr), hdr.data(), sizeof(hdr));
    }
    vtfpp::VTF::CreationOptions options;
    options.outputFormat = ImageFormat::RGBA32323232F;
    options.computeMips = false;
    options.computeThumbnail = false;
    auto vtf = vtfpp::VTF::create(pixels, ImageFormat::RGBA32323232F, 4, 4, options);
    auto decoded = decodeTexture(vtf.bake());
    REQUIRE(decoded);
    CHECK(decoded->format == TextureFormat::Rgba32Float);
    CHECK(decoded->mips.front().data == pixels);
}
