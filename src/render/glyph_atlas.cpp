#include "render/glyph_atlas.hpp"

#include <cmath>

namespace apex::render {

Result<std::shared_ptr<GlyphAtlas>> GlyphAtlas::create(Context& context, TextureHeap& heap, const GlyphAtlasData& data) {
    if (!data.width || !data.height || data.pixels.size() != std::uint64_t(data.width) * data.height ||
        !std::isfinite(data.lineHeight) || data.lineHeight <= 0.0f)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid glyph atlas dimensions"});
    for (const auto& glyph : data.glyphs) {
        const auto& r = glyph.rect;
        if (std::uint64_t(r[0]) + r[2] > data.width || std::uint64_t(r[1]) + r[3] > data.height ||
            !std::isfinite(glyph.advance) || !std::isfinite(glyph.bearing.x) || !std::isfinite(glyph.bearing.y))
            return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid glyph metrics"});
    }
    auto slot = heap.upload({data.width, data.height, VK_FORMAT_R8_UNORM}, data.pixels);
    if (!slot) return std::unexpected(slot.error());
    if (auto flushed = heap.flush(); !flushed) {
        heap.release(*slot);
        return std::unexpected(flushed.error());
    }
    auto atlas = std::shared_ptr<GlyphAtlas>(new GlyphAtlas);
    atlas->context_ = &context;
    atlas->heap_ = &heap;
    atlas->slot_ = *slot;
    atlas->data_ = data;
    atlas->data_.pixels.clear();
    return atlas;
}

GlyphAtlas::~GlyphAtlas() {
    context_->waitIdle();
    heap_->release(slot_);
    heap_->collectGarbage();
}

} // namespace apex::render
