#pragma once

#include "editor/transform.hpp"

#include <cstdint>

namespace apex::editor {

// Transient draw state, outside the document and ChangeSets. Cancel clears it; callers also
// clear it in the same tick as commitTransform(). Equal writes and empty clears are no-ops.
class Preview {
public:
    using Transforms = std::map<NodeId, DMat4>;

    Preview() = default;
    Preview(const Preview&) = default;
    // Replacing an existing channel (including fromTransform()) preserves its revision clock.
    Preview& operator=(const Preview& other);

    const Transforms& transforms() const { return transforms_; }
    std::uint64_t revision() const { return revision_; }
    bool empty() const { return transforms_.empty(); }
    void set(NodeId node, const Transform& transform);
    void set(const Transforms& transforms);
    void clear(NodeId node);
    void clear();

private:
    Transforms transforms_;
    std::uint64_t revision_ = 0;
};

// Containers are retained as IDs: the render mirror applies their transform to descendants.
Preview fromTransform(std::span<const NodeId> nodes, const Transform& transform);

} // namespace apex::editor
