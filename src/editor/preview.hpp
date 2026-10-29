#pragma once

#include "editor/transform.hpp"
#include "geometry/convex_polyhedron.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace apex::editor {

struct GhostSolid {
    geometry::ConvexPolyhedron poly;
    std::string material; // Empty uses the polyhedron's per-face materials.
    float opacity = 0.35f;
    Vec4 outline{1.0f, 0.65f, 0.15f, 1.0f};
};

// Transient draw state, outside the document and ChangeSets. Cancel clears it; callers also
// clear it in the same tick as commitTransform(). Equal writes and empty clears are no-ops.
class Preview {
public:
    using Transforms = std::map<NodeId, DMat4>;
    using EntityKeys = std::map<NodeId, std::map<std::string, std::string>>;
    using Replacements = std::map<NodeId, geometry::ConvexPolyhedron>;

    Preview() = default;
    Preview(const Preview&) = default;
    // Replacing an existing channel (including fromTransform()) preserves its revision clock.
    Preview& operator=(const Preview& other);

    const EntityKeys& entityKeys() const { return entityKeys_; }
    void setEntityKeys(const EntityKeys& keys);
    const Transforms& transforms() const { return transforms_; }
    const Replacements& replacements() const { return replacements_; }
    const std::vector<GhostSolid>& ghosts() const { return ghosts_; }
    std::uint64_t revision() const { return revision_; }
    bool empty() const { return entityKeys_.empty() && transforms_.empty() && replacements_.empty() && ghosts_.empty(); }
    void set(NodeId node, const Transform& transform);
    void set(const Transforms& transforms);
    void setReplacement(NodeId node, const geometry::ConvexPolyhedron& poly);
    void setReplacements(const Replacements& replacements);
    void setGhosts(const std::vector<GhostSolid>& ghosts);
    void clear(NodeId node);
    void clear();

private:
    EntityKeys entityKeys_;
    Transforms transforms_;
    Replacements replacements_;
    std::vector<GhostSolid> ghosts_;
    std::uint64_t revision_ = 0;
};

// Containers are retained as IDs: the render mirror applies their transform to descendants.
Preview fromTransform(std::span<const NodeId> nodes, const Transform& transform);

} // namespace apex::editor
