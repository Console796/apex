#pragma once

// Plan 0.8 D10: the render mirror's overlays. Holds one vertex/index buffer pair per overlay-like
// entity (info_overlay, info_overlay_transition, infodecal), rebuilt on the main thread from the
// document when a ChangeSet touches the entity or a solid it lies on. Draws reuse the overlay's
// material, sit on the layer bias of an alpha-tested face (so they win the depth test against the
// face under them), and carry that face's normal, so Lit shades them like the face.
//
// RenderMirror owns one and calls apply(), refreshDecals() and draw() from its own apply(),
// pump() and buildDrawList(); nothing here touches the document outside apply().

#include "document/document.hpp"
#include "document/overlay_fragments.hpp"
#include "render/renderer.hpp"
#include "render/resources.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace apex::render {

class OverlayMirror {
public:
    using Upload = std::function<Result<Buffer>(std::span<const std::byte> bytes, VkBufferUsageFlags usage)>;
    // World size of a decal material, or nullopt while its texture is still loading.
    using DecalSize = std::function<std::optional<DVec2>(const std::string& material)>;
    struct Material {
        TextureSlot slot = kCheckerboardSlot;
        AlphaMode alpha = AlphaMode::Opaque;
    };
    struct DrawHooks {
        std::function<Material(const std::string& material)> material;
        // Preview index for a piece on `solid` of overlay `overlay`, or nullopt to skip it
        // (hidden or replaced by a preview mesh).
        std::function<std::optional<std::uint32_t>(document::NodeId overlay, document::NodeId solid, DVec3 origin)> place;
        std::function<Vec4(document::NodeId overlay)> highlight;
    };

    // Recomputes the overlays the ChangeSet touches. Retired buffers go to `garbage`. Returns true
    // when anything drawn changed.
    bool apply(const document::Document& doc, const document::ChangeSet& changes, const Upload& upload,
               const DecalSize& decalSize, std::vector<Buffer>& garbage);
    // Re-places decals whose material size has become known (or changed) since they were built.
    bool refreshDecals(const Upload& upload, const DecalSize& decalSize, std::vector<Buffer>& garbage);
    // Appends one MeshDraw per (overlay, solid) piece to `meshes`.
    void draw(std::vector<MeshDraw>& meshes, const DrawHooks& hooks) const;
    void clear(std::vector<Buffer>& garbage);

    std::size_t overlayCount() const { return overlays_.size(); }
    std::size_t pieceCount() const;
    // Every material an overlay draws with (lower case), so the mirror can keep it loaded.
    std::set<std::string> materials() const;

private:
    struct Range {
        document::NodeId solid = 0;
        std::uint32_t firstIndex = 0, indexCount = 0;
        AABB bounds; // Draw-local.
    };
    struct State {
        std::string material;
        DVec3 origin{0.0};
        Buffer vertices, indices;
        std::vector<Range> ranges;
        std::set<document::NodeId> solids; // Solids it lies on or was resolved against.
        bool hidden = false;
        bool unresolved = false; // Lists a side the document did not have.
        // infodecal only: the entity, its candidate faces and the size it was built with.
        std::shared_ptr<const kv1::Node> decal;
        std::vector<document::OverlaySurface> candidates;
        std::optional<DVec2> decalSize;
        std::optional<DVec3> decalOrigin;
    };
    void build(document::NodeId id, State& state, const document::OverlayGeometry& geometry, const Upload& upload,
               std::vector<Buffer>& garbage);
    static void retire(State& state, std::vector<Buffer>& garbage);
    std::map<document::NodeId, State> overlays_;
    std::map<Handle<document::Entity>, document::NodeId> ids_;
};

} // namespace apex::render
