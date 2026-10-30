// Prints a VTF's format and, per face of mip 0, the mean and max of each channel. Every format is
// converted to float RGBA: float formats keep their stored (linear) values; 8-bit and block formats give
// 0..1 values, and colour is additionally shown decoded by gamma 2.2 (the renderer's base-texture decode).
// --faces DIR also writes each face of mip 0 as DIR/<file stem>_<face>.png in stored orientation; float faces
// are normalised by their RGB max and square-rooted for viewing.
#include <vtfpp/VTF.h>
#include <vtfpp/ImageFormats.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vtfpp/ImageConversion.h>

int main(int argc, char** argv) {
    std::filesystem::path facesDir;
    int first = 1;
    if (argc > 2 && std::string_view(argv[1]) == "--faces") {
        facesDir = argv[2];
        first = 3;
    }
    if (argc <= first) {
        std::cerr << "Usage: apex_vtf_stats [--faces DIR] FILE.vtf...\n";
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc; ++i) {
        std::ifstream stream(argv[i], std::ios::binary);
        const std::vector<char> chars{std::istreambuf_iterator<char>(stream), {}};
        if (chars.empty()) { std::cerr << argv[i] << ": cannot read\n"; status = 1; continue; }
        std::vector<std::byte> bytes(chars.size());
        std::memcpy(bytes.data(), chars.data(), chars.size());
        try {
            vtfpp::VTF vtf(bytes);
            if (!vtf) { std::cerr << argv[i] << ": invalid VTF\n"; status = 1; continue; }
            const auto format = vtf.getFormat();
            const bool isFloat = vtfpp::ImageFormatDetails::decimal(format);
            const auto pixels = std::size_t(vtf.getWidth()) * vtf.getHeight();
            std::cout << argv[i] << ": " << vtf.getWidth() << 'x' << vtf.getHeight() << " faces=" << int(vtf.getFaceCount())
                      << " mips=" << int(vtf.getMipCount()) << " format=" << int(format) << (isFloat ? " float" : "")
                      << " srgb=" << vtf.isSRGB() << '\n';
            for (std::uint8_t face = 0; face < vtf.getFaceCount(); ++face) {
                const auto data = vtf.getImageDataAs(vtfpp::ImageFormat::RGBA32323232F, 0, 0, face, 0);
                if (data.size() != pixels * 16) { std::cerr << "  face " << int(face) << ": conversion failed\n"; status = 1; continue; }
                std::array<double, 4> sum{}, peak{};
                std::array<double, 3> gamma{};
                for (std::size_t p = 0; p < pixels; ++p) {
                    for (int c = 0; c < 4; ++c) {
                        float value;
                        std::memcpy(&value, data.data() + (p * 4 + c) * 4, 4);
                        sum[c] += value;
                        peak[c] = std::max<double>(peak[c], value);
                        if (c < 3) gamma[c] += std::pow(std::clamp<double>(value, 0, 1), 2.2);
                    }
                }
                std::cout << "  face " << int(face) << " mean rgba " << sum[0] / double(pixels) << ' ' << sum[1] / double(pixels) << ' '
                          << sum[2] / double(pixels) << ' ' << sum[3] / double(pixels) << "  max " << peak[0] << ' ' << peak[1] << ' '
                          << peak[2] << ' ' << peak[3];
                if (!isFloat) std::cout << "  gamma-decoded rgb " << gamma[0] / double(pixels) << ' ' << gamma[1] / double(pixels) << ' ' << gamma[2] / double(pixels);
                std::cout << '\n';
                if (!facesDir.empty()) {
                    const auto out = facesDir / (std::filesystem::path(argv[i]).stem().string() + "_" + std::to_string(face) + ".png");
                    // 8-bit faces as stored; float faces normalised by their RGB max then square-rooted, for viewing.
                    std::vector<std::byte> rgba(pixels * 4);
                    const double scale = std::max({peak[0], peak[1], peak[2], 1e-12});
                    for (std::size_t p = 0; p < pixels * 4; ++p) {
                        float value;
                        std::memcpy(&value, data.data() + p * 4, 4);
                        const double v = p % 4 == 3 ? 1.0 : std::clamp<double>(isFloat ? std::sqrt(std::max(value / scale, 0.0)) : value, 0, 1);
                        rgba[p] = std::byte(static_cast<unsigned char>(std::lround(v * 255)));
                    }
                    const auto png = vtfpp::ImageConversion::convertImageDataToFile(rgba, vtfpp::ImageFormat::RGBA8888,
                        vtf.getWidth(), vtf.getHeight(), vtfpp::ImageConversion::FileFormat::PNG);
                    std::ofstream file(out, std::ios::binary);
                    file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
                    if (png.empty() || !file) {
                        std::cerr << "  face " << int(face) << ": cannot write " << out << '\n';
                        status = 1;
                    }
                }
            }
        } catch (const std::exception& error) {
            std::cerr << argv[i] << ": " << error.what() << '\n';
            status = 1;
        }
    }
    return status;
}
