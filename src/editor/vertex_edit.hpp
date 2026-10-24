#pragma once

#include "editor/transform.hpp"
#include "geometry/face_matching.hpp"

namespace apex::editor {

struct VertexEditResult {
    geometry::ConvexPolyhedron poly;
    geometry::FaceCorrespondence correspondence{std::in_place};
    // Original vertex indices represented by each output vertex, including welded points.
    std::vector<std::vector<std::size_t>> vertexSources;
    std::vector<std::size_t> absorbedVertices;
    std::vector<std::vector<std::size_t>> mergedVertices;
    std::vector<std::size_t> splitFaces;
};

// Rebuilds from the immutable source and the selected source vertex indices. The result is
// suitable for both preview and Transaction::rebuildSolid; no IDs are allocated here.
Result<VertexEditResult> evaluateVertexEdit(const geometry::ConvexPolyhedron& source,
                                            std::span<const std::size_t> selected,
                                            const Transform& transform);

} // namespace apex::editor
