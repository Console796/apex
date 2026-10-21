#pragma once

// CPU meshing of one VMF solid block (plan 0.1, 0.1.9). Pure and thread-safe: it works on a
// value copy of the block, so it runs on JobPool workers (D12).

#include "core/kv1.hpp"
#include "core/math.hpp"
#include "core/result.hpp"
#include "render/renderer.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace apex::render {

struct TextureSize {
    int width = 64;
    int height = 64;
    bool operator==(const TextureSize&) const = default;
};

// Faces of one material, in one batch. Vertices are relative to SolidMesh::origin.
struct BatchMesh {
    std::string material; // Lower-case.
    std::vector<BrushVertex> vertices;
    std::vector<std::uint32_t> indices; // Triangle list.
    std::vector<std::uint32_t> edges;   // Line list.
};

struct SolidMesh {
    DVec3 origin{0.0};
    AABB bounds;
    std::vector<BatchMesh> batches;
};

// Lower-case material names of the block's sides (sorted, unique).
std::vector<std::string> solidMaterials(const kv1::Node& solidBlock);

// `sizeOf` maps a lower-case material name to its texture size (for UV normalization).
// Fails when the sides do not bound a valid convex solid.
Result<SolidMesh> meshSolid(const kv1::Node& solidBlock,
                            const std::function<TextureSize(std::string_view)>& sizeOf);

} // namespace apex::render
