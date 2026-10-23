#pragma once

#include "render/glyph_atlas.hpp"

namespace apex::render::testing {

// Apex-authored 5x7 bitmap fixture. Unlisted Latin-1 characters use the framed fallback.
inline GlyphAtlasData bitmapFont() {
    GlyphAtlasData data;
    data.width = 16 * 8;
    data.height = 16 * 10;
    data.lineHeight = 10;
    data.pixels.resize(data.width * data.height);
    for (unsigned code = 32; code < 256; ++code) {
        const unsigned x = code % 16 * 8 + 1, y = code / 16 * 10 + 1;
        auto& glyph = data.glyphs[code];
        glyph.advance = 6;
        glyph.bearing = {0, -7};
        glyph.rect = {x, y, 5, 7};
        std::array<unsigned, 7> rows{31, 17, 21, 21, 21, 17, 31};
        switch (code) {
        case ' ': rows = {}; break;
        case 'A': rows = {14, 17, 17, 31, 17, 17, 17}; break;
        case 'p': rows = {0, 0, 30, 17, 30, 16, 16}; break;
        case 'e': rows = {0, 0, 14, 17, 31, 16, 14}; break;
        case 'x': rows = {0, 0, 17, 10, 4, 10, 17}; break;
        case 0xE9: rows = {2, 4, 14, 17, 31, 16, 14}; break;
        }
        for (unsigned row = 0; row < 7; ++row)
            for (unsigned col = 0; col < 5; ++col)
                if (rows[row] & (1u << (4 - col)))
                    data.pixels[(y + row) * data.width + x + col] = std::byte{255};
    }
    return data;
}

} // namespace apex::render::testing
