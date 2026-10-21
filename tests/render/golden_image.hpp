#pragma once

// Golden-image support for GPU tests (plan 0.1, 0.1.9 / D20).
//
// Goldens are stored as 8-bit RGBA PNGs written with stored (uncompressed) deflate blocks, so no
// zlib is needed; only that subset is read back. They are generated on lavapipe.
//
//   APEX_UPDATE_GOLDEN=1       rewrite goldens instead of comparing
//   APEX_GOLDEN_TOLERANCE=N    per-channel difference (0-255) a pixel may have and still match
//   APEX_GOLDEN_BAD_FRACTION=F fraction (0-1) of pixels allowed to exceed the tolerance
//
// Defaults: exact match on llvmpipe; tolerance 12 with 2% of pixels free elsewhere (RADV, ANV).
// On a mismatch the actual image is written next to the temp directory for inspection.

#include "render/context.hpp"
#include "render/render_target.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace golden {

using apex::render::Rgba8Image;

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

inline void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
}

inline void chunk(std::vector<std::uint8_t>& out, const char type[5], const std::vector<std::uint8_t>& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()));
}

inline std::vector<std::uint8_t> encodePng(const Rgba8Image& image) {
    std::vector<std::uint8_t> raw;
    const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), image.pixels.begin() + static_cast<std::ptrdiff_t>(y * stride),
                   image.pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * stride));
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    for (std::size_t pos = 0; pos < raw.size() || pos == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n & 0xFF));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
        z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        if (last) break;
    }
    put32(z, (b << 16) | a);
    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, image.width);
    put32(ihdr, image.height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

inline std::optional<Rgba8Image> decodePng(const std::vector<std::uint8_t>& bytes) {
    const auto be32 = [&](std::size_t at) {
        return (std::uint32_t(bytes[at]) << 24) | (std::uint32_t(bytes[at + 1]) << 16) |
               (std::uint32_t(bytes[at + 2]) << 8) | bytes[at + 3];
    };
    if (bytes.size() < 8 + 25 || bytes[1] != 'P') return std::nullopt;
    Rgba8Image image;
    std::vector<std::uint8_t> z;
    for (std::size_t at = 8; at + 12 <= bytes.size();) {
        const std::uint32_t length = be32(at);
        const std::string type(bytes.begin() + static_cast<std::ptrdiff_t>(at + 4), bytes.begin() + static_cast<std::ptrdiff_t>(at + 8));
        if (at + 12 + length > bytes.size()) return std::nullopt;
        const auto* data = bytes.data() + at + 8;
        if (type == "IHDR") {
            if (length != 13 || data[8] != 8 || data[9] != 6 || data[12] != 0) return std::nullopt;
            image.width = be32(at + 8);
            image.height = be32(at + 12);
        } else if (type == "IDAT") {
            z.insert(z.end(), data, data + length);
        }
        at += 12 + length;
    }
    std::vector<std::uint8_t> raw;
    for (std::size_t pos = 2; pos + 5 <= z.size();) {
        const bool last = z[pos] & 1;
        if ((z[pos] & 6) != 0) return std::nullopt; // Only stored blocks.
        const std::size_t n = z[pos + 1] | (std::size_t(z[pos + 2]) << 8);
        if (pos + 5 + n > z.size()) return std::nullopt;
        raw.insert(raw.end(), z.begin() + static_cast<std::ptrdiff_t>(pos + 5), z.begin() + static_cast<std::ptrdiff_t>(pos + 5 + n));
        pos += 5 + n;
        if (last) break;
    }
    const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
    if (image.width == 0 || raw.size() != (stride + 1) * image.height) return std::nullopt;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        if (raw[y * (stride + 1)] != 0) return std::nullopt;
        image.pixels.insert(image.pixels.end(), raw.begin() + static_cast<std::ptrdiff_t>(y * (stride + 1) + 1),
                            raw.begin() + static_cast<std::ptrdiff_t>((y + 1) * (stride + 1)));
    }
    return image;
}

inline std::optional<std::string> env(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) return std::nullopt;
    return std::string(value);
}

struct Tolerance {
    int channel = 0;
    double badFraction = 0.0;
};

inline Tolerance toleranceFor(const std::string& deviceName) {
    std::string lowered = deviceName;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool software = lowered.find("llvmpipe") != std::string::npos;
    Tolerance t{software ? 0 : 12, software ? 0.0 : 0.02};
    if (const auto v = env("APEX_GOLDEN_TOLERANCE")) t.channel = std::atoi(v->c_str());
    if (const auto v = env("APEX_GOLDEN_BAD_FRACTION")) t.badFraction = std::atof(v->c_str());
    return t;
}

// Compares `actual` with GOLDEN_DIR/<name>.png (or rewrites it under APEX_UPDATE_GOLDEN=1).
inline void check(const std::string& name, const Rgba8Image& actual, const apex::render::Context& context) {
    namespace fs = std::filesystem;
    const fs::path path = fs::path(APEX_GOLDEN_DIR) / (name + ".png");
    const auto writePng = [&](const fs::path& to) {
        fs::create_directories(to.parent_path());
        const auto bytes = encodePng(actual);
        std::ofstream(to, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                  static_cast<std::streamsize>(bytes.size()));
    };
    if (env("APEX_UPDATE_GOLDEN")) {
        if (context.info().name.find("llvmpipe") == std::string::npos) {
            MESSAGE("warning: updating golden '" << name << "' on non-lavapipe device " << context.info().name);
        }
        writePng(path);
        MESSAGE("golden updated: " << path.string());
        return;
    }
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "missing golden " << path.string() << " (run with APEX_UPDATE_GOLDEN=1 on lavapipe)");
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto expected = decodePng(bytes);
    REQUIRE_MESSAGE(expected.has_value(), "unreadable golden " << path.string());
    REQUIRE(expected->width == actual.width);
    REQUIRE(expected->height == actual.height);
    const Tolerance tolerance = toleranceFor(context.info().name);
    std::size_t bad = 0;
    int worst = 0;
    for (std::size_t i = 0; i < actual.pixels.size(); i += 4) {
        int diff = 0;
        for (int c = 0; c < 4; ++c) diff = std::max(diff, std::abs(int(actual.pixels[i + c]) - int(expected->pixels[i + c])));
        worst = std::max(worst, diff);
        bad += diff > tolerance.channel ? 1 : 0;
    }
    const double fraction = static_cast<double>(bad) / static_cast<double>(actual.pixels.size() / 4);
    const bool ok = fraction <= tolerance.badFraction;
    if (!ok) {
        const fs::path out = fs::temp_directory_path() / "apex-golden-actual" / (name + ".png");
        writePng(out);
        MESSAGE("golden '" << name << "' mismatch on " << context.info().name << ": " << bad << " pixels beyond "
                           << tolerance.channel << " (worst diff " << worst << "); actual written to " << out.string());
    }
    CHECK(ok);
}

} // namespace golden
