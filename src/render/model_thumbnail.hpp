#pragma once

#include "assets/material_cache.hpp"
#include "assets/model_data.hpp"
#include "assets/vtf.hpp"
#include "render/render_target.hpp"
#include "render/renderer.hpp"

#include <chrono>
#include <list>
#include <map>

namespace apex::render {

// The loaded materials of a model's draw ranges, by the range's material index. `name` identifies the
// texture contents for the thumbnail texture LRU. Ranges without an entry draw flat.
struct ModelMaterial { std::string name; std::shared_ptr<const assets::CachedMaterial> material; };
using ModelMaterials = std::map<std::int32_t, ModelMaterial>;

// One owner thread and one thumbnail renderer per heap. Context and heap outlive it.
// Skin 0, body 0; 128px RGBA with transparent neutral background. The orthographic camera looks
// down 25 degrees at yaw 135 degrees, with 10% padding around default-body bounds. Ranges whose
// material is supplied draw textured (one mip, largest side at most kMaxTextureSize, at most
// kThumbnailTextureSlots - 1 distinct materials); the rest draw as a flat tint. With no usable
// material at all the whole image is flat.
class ModelThumbnailRenderer {
public:
    static constexpr int kSize = 128;
    static Result<std::unique_ptr<ModelThumbnailRenderer>> create(Context& context, TextureHeap& heap);
    ~ModelThumbnailRenderer();
    static constexpr std::uint32_t kMaxTextureSize = 256;
    Result<assets::Thumbnail> render(const assets::ModelData& model, const ModelMaterials& materials = {});
    // Names identify immutable texture contents. Hits touch the LRU; misses upload into the
    // reserved range, replacing its oldest entry after 64 textures.
    Result<TextureSlot> texture(std::string_view name, const TextureDesc& desc, std::span<const std::byte> pixels);
    std::uint32_t maxThumbnailSlotsLive() const { return heap_.maxThumbnailSlotsLive(); }
    // Includes geometry preparation/upload, render submission and synchronous RGBA readback.
    std::chrono::nanoseconds lastDuration() const { return lastDuration_; }
private:
    ModelThumbnailRenderer(Context& context, TextureHeap& heap, TextureSlot first,
                           RenderTarget target, std::unique_ptr<Renderer> renderer);
    Context& context_;
    TextureHeap& heap_;
    TextureSlot first_;
    RenderTarget target_;
    std::unique_ptr<Renderer> renderer_;
    struct Texture { std::string name; TextureSlot slot; };
    std::list<Texture> textures_;
    std::chrono::nanoseconds lastDuration_{};
};

} // namespace apex::render
