#pragma once

#include "lighting/bake_scene.hpp"
#include "render/texture_heap.hpp"

#include <map>

namespace apex::render {

struct LightmapRect {
    int x = 0, y = 0, width = 0, height = 0; // Interior; one duplicated texel surrounds each rect.
    bool operator==(const LightmapRect&) const = default;
};

// Indices are BakeScene::faces / Baker::resolve() indices, including separate chart fragments.
// Source identity is (document NodeId, SideId); fragment order is retained from BakeScene.
struct LightmapAtlas {
    int size = 0;
    std::vector<lighting::BakeFace> faces;
    std::vector<LightmapRect> rects;
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::vector<std::size_t>> fragments;

    static Result<LightmapAtlas> pack(std::span<const lighting::BakeFace> faces, int maxSize = 4096);
    DVec2 uv(std::size_t face, DVec3 world) const;
};

class BakedLightmaps {
  public:
    static Result<std::shared_ptr<BakedLightmaps>> create(Context& context, TextureHeap& heap,
                                                         LightmapAtlas atlas);
    ~BakedLightmaps();
    const LightmapAtlas& atlas() const { return *atlas_; }
    std::shared_ptr<const LightmapAtlas> layout() const { return atlas_; }
    TextureSlot slot() const { return slot_; }
    // Empty selection uploads all faces; otherwise only the named resolve() rows and their guards.
    // Main thread, between frames. Synchronous: waits for prior draws, stages selected rectangles,
    // and completes their copies before returning. Layout/UVs and slot remain stable.
    Result<void> upload(const std::vector<std::vector<Vec3>>& radiance,
                        std::span<const std::size_t> faces = {});

  private:
    BakedLightmaps(Context& context, TextureHeap& heap, LightmapAtlas atlas);
    Context& context_;
    TextureHeap& heap_;
    std::shared_ptr<const LightmapAtlas> atlas_;
    Image image_;
    TextureSlot slot_ = kNoTexture;
};

} // namespace apex::render
