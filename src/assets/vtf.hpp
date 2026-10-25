#pragma once

#include "assets/vfs.hpp"

#include <array>
#include <span>

namespace apex::assets {

enum class TextureFormat { Rgba8, Rgba32Float, Bc1, Bc2, Bc3, Bc4, Bc5, Bc6Signed, Bc6Unsigned, Bc7 };
struct TextureMip {
    std::uint16_t width = 0, height = 0, depth = 1;
    // frame-major, then cube face, then depth slice. Each slice is tightly packed.
    Blob data;
};
struct Texture {
    TextureFormat format = TextureFormat::Rgba8;
    bool srgb = false;
    std::uint16_t frames = 1;
    std::uint8_t faces = 1;
    std::array<float, 3> reflectivity{};
    // Largest mip first. AXC is inflated; BC blocks remain compressed for GPU upload.
    std::vector<TextureMip> mips;
    std::size_t byteSize() const;
};
struct Thumbnail {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
// Smallest mip at or above size, frame/face/slice zero, box-filtered until its larger side is
// under 2 x size. HDR is clamped to [0, 1].
Result<Thumbnail> decodeThumbnail(std::span<const std::byte> bytes, int size);
Result<Texture> decodeTexture(std::span<const std::byte> bytes);
Result<Texture> loadTexture(const Vfs& vfs, std::string_view name);
Texture checkerboardTexture();

} // namespace apex::assets
