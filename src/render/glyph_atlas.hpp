#pragma once

#include "render/texture_heap.hpp"
#include "core/math.hpp"

#include <array>
#include <memory>

namespace apex::render {

struct GlyphMetrics {
    float advance = 0.0f;
    Vec2 bearing{0.0f}; // Top-left offset from the baseline, pixels, y down.
    std::array<std::uint32_t, 4> rect{}; // Atlas x, y, width, height.
};

struct GlyphAtlasData {
    std::uint32_t width = 0, height = 0;
    std::vector<std::byte> pixels; // Tightly packed R8 coverage; ASCII + Latin-1 code points.
    std::array<GlyphMetrics, 256> glyphs{};
    float lineHeight = 0.0f;
};

class GlyphAtlas {
public:
    static Result<std::shared_ptr<GlyphAtlas>> create(Context& context, TextureHeap& heap, const GlyphAtlasData& data);
    ~GlyphAtlas();
    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;
    TextureSlot slot() const { return slot_; }
    const GlyphAtlasData& data() const { return data_; }

private:
    GlyphAtlas() = default;
    Context* context_ = nullptr;
    TextureHeap* heap_ = nullptr;
    TextureSlot slot_ = kCheckerboardSlot;
    GlyphAtlasData data_;
};

} // namespace apex::render
