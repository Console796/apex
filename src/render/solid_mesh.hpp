#pragma once

// CPU meshing of one VMF solid block (plan 0.1, 0.1.9). Pure and thread-safe: it works on a
// value copy of the block, so it runs on JobPool workers (D12).

#include "core/kv1.hpp"
#include "core/math.hpp"
#include "core/result.hpp"
#include "document/document.hpp"
#include "document/instance_contents.hpp"
#include "geometry/convex_polyhedron.hpp"
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
    document::SideId side = 0;
    bool displacement = false; // Baked retains the existing Lit path for unsupported displacement sides.
    std::uint32_t firstIndex = 0, indexCount = 0;
    std::uint32_t firstEdgeIndex = 0, edgeIndexCount = 0;
};

// Faces of one material, in one batch. Vertices are relative to SolidMesh::origin.
struct BatchMesh {
    std::string material; // Lower-case.
    std::vector<BrushVertex> vertices;
    std::vector<std::uint32_t> indices; // Triangle list.
    std::vector<std::uint32_t> edges;   // Line list.
    std::vector<FaceRange> faces;
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

// Same mesher for preview values and parsed committed solids. Face material and UV data are used
// verbatim; invalid polyhedra fail before any vertices are emitted.
Result<SolidMesh> meshSolid(const geometry::ConvexPolyhedron& poly,
                            const std::function<TextureSize(std::string_view)>& sizeOf,
                            const LightmapAtlas* lightmaps = nullptr, std::uint64_t solidId = 0,
                            const document::InstanceSolidOrigin* origin = nullptr);

} // namespace apex::render
