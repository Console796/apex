#pragma once

#include "geometry/bvh.hpp"

#include <utility>
#include <vector>

namespace apex::editor {

struct PickTriangleRange {
    std::uint32_t firstTriangle = 0, triangleCount = 0;
};
struct PickModelChoice {
    std::vector<PickTriangleRange> ranges;
};
struct PickModelBodyPart {
    std::uint32_t base = 1;
    std::vector<PickModelChoice> choices;
};

// Construct once (also safe on a worker), then share as const. Ranges partition the
// triangles when body parts are supplied; without body parts every triangle is active.
class PickModel {
public:
    PickModel(std::vector<DVec3> positions, std::vector<std::uint32_t> indices,
              std::vector<PickModelBodyPart> bodyParts = {});
    const geometry::Bvh& triangleBvh() const { return tree_; }
    AABB bounds() const { return tree_.bounds(); }
    AABB bounds(std::uint32_t body) const;

private:
    friend class PickIndex;
    std::vector<DVec3> positions_;
    std::vector<std::uint32_t> indices_;
    std::vector<PickModelBodyPart> bodyParts_;
    std::vector<std::vector<AABB>> choiceBounds_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> owners_;
    geometry::Bvh tree_;

    bool selected(std::uint32_t triangle, std::uint32_t body) const;
};

} // namespace apex::editor
