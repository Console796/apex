#pragma once

#include <vtfpp/VTF.h>

namespace {
vtfpp::VTF whiteVtf(vtfpp::ImageFormat format = vtfpp::ImageFormat::RGBA8888,
                    std::uint32_t version = 6) {
    std::vector<std::byte> white(8 * 8 * 4, std::byte{255});
    vtfpp::VTF::CreationOptions options;
    options.version = version;
    options.outputFormat = format;
    options.computeThumbnail = false;
    options.computeReflectivity = false;
    // The pinned writer inverts BGRA5551 alpha, truncates 10-bit colour, and has
    // no RGBA -> UVWQ conversion. Supply known encoded pixels for those fixtures.
    const bool encoded = format == vtfpp::ImageFormat::BGRA5551 ||
                         format == vtfpp::ImageFormat::RGBA1010102 ||
                         format == vtfpp::ImageFormat::BGRA1010102 ||
                         format == vtfpp::ImageFormat::UVWQ8888;
    if (encoded) {
        white.assign(vtfpp::ImageFormatDetails::getDataLength(format, 8, 8), std::byte{255});
        options.computeMips = false;
    }
    auto vtf = vtfpp::VTF::create(white, encoded ? format : vtfpp::ImageFormat::RGBA8888,
                                8, 8, options);
    vtf.setReflectivity({0.125f, 0.25f, 0.5f});
    return vtf;
}
} // namespace
