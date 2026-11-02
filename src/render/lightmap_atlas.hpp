#pragma once

#include "document/instance_contents.hpp"
#include "lighting/bake_diff.hpp"
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
    // Live editor only: project retained radiance onto edited geometry until rebaked.
    bool retainStale = false;
    // Faces are compiler (VRAD) fragments, not the baker's charts: the mesher keeps a side's fragments
    // while they still lie on the side and cover no more than it, instead of requiring the baker's chart.
    bool compiled = false;
    int size = 0;
    std::vector<lighting::BakeFace> faces;
    std::vector<LightmapRect> rects;
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::vector<std::size_t>> fragments;
    // Baked faces made from instance contents, by placement then template solid id. The viewport
    // draws the template itself, so it finds a face by template ids; `fragments` stays keyed by the
    // expanded snapshot's ids. Only solids with at least one baked side are present.
    std::map<document::InstancePlacement, std::map<int, document::InstanceSolidOrigin>> instances;
    // Expanded (solid, side) <-> template identity, so a restart after an edit (which renumbers the
    // expansion) still finds the previous radiance of the same placed face.
    struct TemplateSide {
        document::InstancePlacement placement;
        int solid = 0, side = 0;
        auto operator<=>(const TemplateSide&) const = default;
    };
    std::map<std::pair<std::uint64_t, std::uint32_t>, TemplateSide> templateOfSide;
    std::map<TemplateSide, std::pair<std::uint64_t, std::uint32_t>> sideOfTemplate;

    static Result<LightmapAtlas> pack(std::span<const lighting::BakeFace> faces, int maxSize = 4096);
    DVec2 uv(std::size_t face, DVec3 world) const;
    // Record where each expanded instance solid came from (expandBakeInstances); call after pack.
    void bindInstances(std::span<const document::InstanceSolidOrigin> origins);
    // Incremental-bake identity per face: instance faces by template placement/solid/side (stable across
    // edits, unlike the expansion's ids), the rest by source solid/side; plus the fragment ordinal.
    std::vector<lighting::FaceKey> faceKeys() const;
    // CPU-only restart seed: nearest old luxel of the same source side, black for new sides.
    std::vector<std::vector<Vec3>> retainRadiance(
        const LightmapAtlas* previous, const std::vector<std::vector<Vec3>>& radiance) const;
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
