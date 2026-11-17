#pragma once

// Plan 0.8.9 D16: the render mirror's detail props. VBSP scatters detail.vbsp's sprites and models on
// displacements whose material sets %detailtype (docs/plans/probe-results/detail_props_0_8_9_p0.md).
// This places them with geometry::placeDetailProps, the port checked against the Windows vbsp, and holds
// one sprite vertex/index buffer per displaced solid, rebuilt on the main thread when a ChangeSet touches
// the solid or once its materials have loaded. Model props are listed for the mirror's instanced model path.
//
// Placement is exact; the drawing is a preview. Every quad is two-sided and lit from above, sprites keep
// their placed angles even when detail.vbsp asks for screen alignment (orientation 1 and 2), and the
// cross and tri shapes are crossed quads in the spirit of the game's, not its exact geometry.

#include "core/math.hpp"
#include "document/document.hpp"
#include "geometry/detail_props.hpp"
#include "geometry/displacement.hpp"
#include "render/overlay_mirror.hpp"
#include "render/renderer.hpp"
#include "render/resources.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace apex::render {

// One displaced side, ready for placement.
struct DetailFace {
    std::array<DVec3, 4> corners; // Counter-clockwise from outside (geometry::displacementCorners).
    geometry::Displacement disp;
    int seed = 0;         // The side's "id": VBSP seeds both random streams with it.
    std::string material; // Lower case.
};
// The displaced sides of a solid block. Unreadable ones are skipped; the Problems checks report them.
std::vector<DetailFace> detailFaces(const kv1::Node& solid);

struct DetailModelProp {
    std::string model; // models/....mdl
    DMat4 transform;
};
struct DetailPlacement {
    DVec3 origin{0.0}; // Sprite vertices are relative to it.
    std::vector<BrushVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<DetailModelProp> models;
    std::size_t props = 0;
    AABB bounds; // Draw-local.
};
// Places one face's props and appends their sprite quads and model transforms. CPU only.
Result<void> appendDetailProps(const DetailFace& face, const geometry::DetailType& type, DetailPlacement& out);

class DetailMirror {
public:
    using Upload = OverlayMirror::Upload;
    // A material's %detailtype: nullopt while it loads, empty when it sets none.
    using DetailTypeOf = std::function<std::optional<std::string>(const std::string& material)>;
    struct DrawHooks {
        TextureSlot slot = kCheckerboardSlot; // The worldspawn's detailmaterial.
        AlphaMode alpha = AlphaMode::Test;
        // Preview index for the props of `solid`, or nullopt to skip them (hidden or replaced by a preview mesh).
        std::function<std::optional<std::uint32_t>(document::NodeId solid, DVec3 origin)> place;
        std::function<Vec4(document::NodeId solid)> highlight;
    };

    // Replaces detail.vbsp (null: none) and re-places every solid at the next refresh().
    void setVbsp(std::shared_ptr<const geometry::DetailVbsp> vbsp);
    const geometry::DetailVbsp* vbsp() const { return vbsp_.get(); }
    // Re-reads the displaced sides of the solids the ChangeSet touches. Returns true when anything drawn changed.
    bool apply(const document::Document& doc, const document::ChangeSet& changes, std::vector<Buffer>& garbage);
    // Places stale solids whose materials have loaded. Returns true when anything drawn changed.
    bool refresh(const DetailTypeOf& detailType, const Upload& upload, std::vector<Buffer>& garbage);
    void draw(std::vector<MeshDraw>& meshes, const DrawHooks& hooks) const;
    // Calls visit(solid, prop) for every placed model prop.
    void forEachModel(const std::function<void(document::NodeId, const DetailModelProp&)>& visit) const;
    void clear(std::vector<Buffer>& garbage);

    std::size_t propCount() const;
    bool pending() const; // A displaced solid still waits for its materials.

private:
    struct State {
        std::vector<DetailFace> faces;
        bool stale = true;
        DVec3 origin{0.0};
        Buffer vertices, indices;
        std::uint32_t indexCount = 0;
        AABB bounds;
        std::vector<DetailModelProp> models;
        std::size_t props = 0;
    };
    static void retire(State& state, std::vector<Buffer>& garbage);

    std::shared_ptr<const geometry::DetailVbsp> vbsp_;
    std::map<document::NodeId, State> solids_;
    std::map<Handle<document::Solid>, document::NodeId> ids_;
};

} // namespace apex::render
