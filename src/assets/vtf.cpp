#include "assets/vtf.hpp"

#include "assets/vmt.hpp"

#include <exception>
#include <limits>
#include <vtfpp/VTF.h>

namespace apex::assets {
namespace {
TextureFormat gpuFormat(vtfpp::ImageFormat format) {
    using enum vtfpp::ImageFormat;
    switch (format) {
    case DXT1: case DXT1_ONE_BIT_ALPHA: return TextureFormat::Bc1;
    case DXT3: return TextureFormat::Bc2;
    case DXT5: return TextureFormat::Bc3;
    case ATI1N: case STRATA_BC4: return TextureFormat::Bc4;
    case ATI2N: case STRATA_BC5: return TextureFormat::Bc5;
    case STRATA_BC6H_SF: return TextureFormat::Bc6Signed;
    case STRATA_BC6H_UF: case TITANFALL_BC6H: return TextureFormat::Bc6Unsigned;
    case STRATA_BC7: case TITANFALL_BC7: return TextureFormat::Bc7;
    default:
        return vtfpp::ImageFormatDetails::large(format) ? TextureFormat::Rgba32Float : TextureFormat::Rgba8;
    }
}
} // namespace
std::size_t Texture::byteSize() const {
    std::size_t bytes = 0;
    for (const auto& mip : mips) {
        bytes += mip.data.size();
    }
    return bytes;
}
Result<Texture> decodeTexture(std::span<const std::byte> bytes) try {
    vtfpp::VTF vtf(bytes);
    if (!vtf || !vtf.hasImageData() || !vtf.getWidth() || !vtf.getHeight() ||
        !vtf.getMipCount() || vtf.getMipCount() > 16 || !vtf.getFrameCount()) {
        return std::unexpected(Error{ErrorCode::ParseError, "Invalid or empty VTF"});
    }
    Texture result;
    result.format = gpuFormat(vtf.getFormat());
    result.srgb = vtf.isSRGB();
    result.frames = vtf.getFrameCount();
    result.faces = vtf.getFaceCount();
    // vtfpp's subresource getter uses unchecked span::subspan. Validate the entire
    // image resource before requesting any slices, including truncated legacy files.
    std::uint64_t required = 0;
    for (std::uint8_t mip = 0; mip < vtf.getMipCount(); ++mip) {
        auto length = vtfpp::ImageFormatDetails::getDataLength(
            vtf.getFormat(), vtf.getWidth(mip), vtf.getHeight(mip));
        if (!length) {
            return std::unexpected(Error{ErrorCode::Unsupported, "Unsupported VTF format"});
        }
        required += std::uint64_t(length) * vtf.getDepth(mip) * result.frames * result.faces;
    }
    const auto* image = vtf.getResource(vtfpp::Resource::TYPE_IMAGE_DATA);
    if (!image || required > std::numeric_limits<std::uint32_t>::max() ||
        required > image->data.size()) {
        return std::unexpected(Error{ErrorCode::ParseError, "Incomplete or oversized VTF image"});
    }
    auto reflectivity = vtf.getReflectivity();
    for (std::uint8_t i = 0; i < 3; ++i) {
        result.reflectivity[i] = reflectivity[i];
    }
    const bool blocks = result.format != TextureFormat::Rgba8 && result.format != TextureFormat::Rgba32Float;
    for (std::uint8_t mip = 0; mip < vtf.getMipCount(); ++mip) {
        TextureMip output{vtf.getWidth(mip), vtf.getHeight(mip), vtf.getDepth(mip), {}};
        for (std::uint16_t frame = 0; frame < result.frames; ++frame) {
            for (std::uint8_t face = 0; face < result.faces; ++face) {
                for (std::uint16_t slice = 0; slice < output.depth; ++slice) {
                    auto raw = vtf.getImageDataRaw(mip, frame, face, slice);
                    const auto expected = vtfpp::ImageFormatDetails::getDataLength(
                        vtf.getFormat(), output.width, output.height);
                    if (!expected || raw.size() != expected) {
                        return std::unexpected(Error{ErrorCode::ParseError, "Incomplete VTF mip"});
                    }
                    if (blocks) {
                        output.data.insert(output.data.end(), raw.begin(), raw.end());
                    } else {
                        auto converted = vtf.getImageDataAs(result.format == TextureFormat::Rgba8 ?
                            vtfpp::ImageFormat::RGBA8888 : vtfpp::ImageFormat::RGBA32323232F, mip, frame, face, slice);
                        const auto size = std::size_t(output.width) * output.height *
                            (result.format == TextureFormat::Rgba8 ? 4 : 16);
                        if (converted.size() != size) {
                            return std::unexpected(
                                Error{ErrorCode::Unsupported, "VTF pixel conversion failed"});
                        }
                        // SourcePP returns alpha=1 for the unused bit in BGRX5551.
                        // X formats are opaque, so normalize it to the RGBA8 alpha range.
                        if (vtf.getFormat() == vtfpp::ImageFormat::BGRX5551) {
                            for (std::size_t i = 3; i < converted.size(); i += 4) {
                                converted[i] = std::byte{255};
                            }
                        }
                        output.data.insert(output.data.end(), converted.begin(), converted.end());
                    }
                }
            }
        }
        result.mips.push_back(std::move(output));
    }
    return result;
} catch (const std::exception& e) {
    return std::unexpected(Error{ErrorCode::ParseError, e.what()});
}
Result<Texture> loadTexture(const Vfs& vfs, std::string_view name) {
    auto blob = vfs.open(texturePath(name));
    if (!blob) {
        return std::unexpected(blob.error());
    }
    auto result = decodeTexture(*blob);
    if (!result) {
        return std::unexpected(result.error().withContext(texturePath(name)));
    }
    return result;
}
Texture checkerboardTexture() {
    Texture result;
    TextureMip mip{8, 8, 1, {}};
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            auto c = std::byte(((x / 4) ^ (y / 4)) ? 255 : 0);
            mip.data.insert(mip.data.end(), {c, std::byte{0}, c, std::byte{255}});
        }
    result.mips.push_back(std::move(mip));
    return result;
}
} // namespace apex::assets
