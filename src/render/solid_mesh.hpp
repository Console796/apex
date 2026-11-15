#pragma once

// CPU meshing of one VMF solid block (plan 0.1, 0.1.9). Pure and thread-safe: it works on a
// value copy of the block, so it runs on JobPool workers (D12).

#include "core/kv1.hpp"
#include "core/math.hpp"
#include "core/result.hpp"
#include "document/document.hpp"
#include "document/instance_contents.hpp"
#include "geometry/convex_polyhedron.hpp"
#include "geometry/displacement.hpp"
#include "render/renderer.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace apex::render {

struct LightmapAtlas;

struct TextureSize {
    int width = 64;
    int height = 64;
    DMat3 uvTransform{1.0};
    bool operator==(const TextureSize&) const = default;
};

// Ranges into the batch's existing buffers; face selection never re-meshes or uploads.
struct FaceRange {
    DVec3 centre{0.0};
    DVec3 normal{0.0}; // Unit outward plane normal.
    document::SideId side = 0;
    bool displacement = false; // document::isDisplaced; Baked keeps the Lit path for these unless:
    bool displacementLightmap = false; // the displaced vertices carry their displaced fragment's UVs (plan 0.8.5).
    std::uint32_t firstIndex = 0, indexCount = 0;
    std::uint32_t firstEdgeIndex = 0, edgeIndexCount = 0;
};

// Faces of one material, in one batch. Vertices are relative to SolidMesh::origin.
// Index layout: [displaced meshes | flat faces | displaced sides' flat base faces]. Draws use
// [0, drawIndexCount); shadow casters and the scene BVH use [casterFirstIndex, indices.size()), so
// a displacement casts as its flat base face, as before displacements were drawn (plan 0.8 D8:
// displaced casters wait for 0.8.5's probe). Without displacements both cover every index.
struct BatchMesh {
    std::string material; // Lower-case.
    std::vector<BrushVertex> vertices;
    std::vector<std::uint32_t> indices; // Triangle list.
    std::vector<std::uint32_t> edges;   // Line list; a displaced side lists every triangle edge.
    std::vector<FaceRange> faces;
    std::uint32_t drawIndexCount = 0;
    std::uint32_t casterFirstIndex = 0;
};

struct SolidMesh {
    DVec3 origin{0.0};
    AABB bounds;
    std::vector<BatchMesh> batches;
};

// Lower-case material names of the block's sides (sorted, unique).
std::vector<std::string> solidMaterials(const kv1::Node& solidBlock);

// Lightmaps of a solid that the viewport draws template-local while the bake saw it expanded: the
// solid's baked ids and the transform it was baked under. Looked up by template side id.
using LightmapPlacement = std::map<int, document::InstanceSolidOrigin>; // By template solid id.

// `sizeOf` maps a lower-case material name to its texture size and material transform (for UV normalization and sampling).
// Fails when the sides do not bound a valid convex solid.
Result<SolidMesh> meshSolid(const kv1::Node& solidBlock,
                            const std::function<TextureSize(std::string_view)>& sizeOf,
                            const LightmapAtlas* lightmaps = nullptr, std::uint64_t solidId = 0,
                            const LightmapPlacement* placement = nullptr);

// Displaced sides (document::isDisplaced) are meshed by geometry::displacementMesh (plan 0.8 D2, D8):
// texture UVs are the side's projection at the flat (undisplaced) grid point, as VBSP and the engine
// interpolate the base face's corner texcoords, normals are area-weighted over the displaced
// triangles, and BrushVertex::alpha carries the clamped paint alpha. A side whose dispinfo does not
// parse, or whose base face is not a quad, draws as its flat face. Bounds include displaced vertices.
// Same mesher for preview values and parsed committed solids. Face material and UV data are used
// verbatim; invalid polyhedra fail before any vertices are emitted. Preview values carry no
// dispinfo, so they mesh flat unless the preview supplies their displacements (the overload below).
Result<SolidMesh> meshSolid(const geometry::ConvexPolyhedron& poly,
                            const std::function<TextureSize(std::string_view)>& sizeOf,
                            const LightmapAtlas* lightmaps = nullptr, std::uint64_t solidId = 0,
                            const document::InstanceSolidOrigin* origin = nullptr);
// A preview value with displaced sides (plan 0.8 D6's sculpt stroke): the sides keyed in
// `displacements` mesh as displaced and hide their solid's other sides, as a parsed block's would.
Result<SolidMesh> meshSolid(const geometry::ConvexPolyhedron& poly,
                            const std::function<TextureSize(std::string_view)>& sizeOf,
                            const std::map<document::SideId, geometry::Displacement>& displacements);

} // namespace apex::render
