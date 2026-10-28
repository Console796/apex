#include "editor/pick_model.hpp"

#include <limits>
#include <stdexcept>

namespace apex::editor {

PickModel::PickModel(std::vector<DVec3> positions, std::vector<std::uint32_t> indices,
                     std::vector<PickModelBodyPart> bodyParts)
    : positions_(std::move(positions)), indices_(std::move(indices)), bodyParts_(std::move(bodyParts)) {
    if (indices_.size() % 3 || indices_.size() / 3 > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("invalid pick model triangle count");
    std::vector<geometry::Bvh::Item> triangles;
    triangles.reserve(indices_.size() / 3);
    for (std::size_t i = 0; i < indices_.size(); i += 3) {
        AABB bounds;
        for (std::size_t j = 0; j < 3; ++j) {
            if (indices_[i + j] >= positions_.size())
                throw std::invalid_argument("pick model index out of range");
            const auto p = positions_[indices_[i + j]];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                throw std::invalid_argument("nonfinite pick model position");
            bounds.expand(p);
        }
        triangles.push_back({static_cast<std::uint32_t>(i / 3), bounds});
    }
    owners_.resize(triangles.size(), {std::numeric_limits<std::uint32_t>::max(), 0});
    choiceBounds_.resize(bodyParts_.size());
    for (std::size_t part = 0; part < bodyParts_.size(); ++part) {
        const auto& choices = bodyParts_[part].choices;
        choiceBounds_[part].resize(choices.size());
        for (std::size_t choice = 0; choice < choices.size(); ++choice) {
            for (const auto range : choices[choice].ranges) {
                if (range.firstTriangle > triangles.size() ||
                    range.triangleCount > triangles.size() - range.firstTriangle)
                    throw std::invalid_argument("pick model triangle range out of bounds");
                for (std::size_t i = range.firstTriangle; i < std::size_t(range.firstTriangle) + range.triangleCount; ++i) {
                    if (owners_[i].first != std::numeric_limits<std::uint32_t>::max())
                        throw std::invalid_argument("overlapping pick model triangle ranges");
                    owners_[i] = {static_cast<std::uint32_t>(part), static_cast<std::uint32_t>(choice)};
                    choiceBounds_[part][choice].expand(triangles[i].bounds);
                }
            }
        }
    }
    if (!bodyParts_.empty()) {
        for (const auto owner : owners_) {
            if (owner.first == std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("unassigned pick model triangle");
        }
    }
    tree_.build(triangles);
}

bool PickModel::selected(std::uint32_t triangle, std::uint32_t body) const {
    if (bodyParts_.empty()) return true;
    const auto [part, choice] = owners_[triangle];
    const auto& group = bodyParts_[part];
    return choice == (body / (group.base ? group.base : 1)) % group.choices.size();
}

AABB PickModel::bounds(std::uint32_t body) const {
    if (bodyParts_.empty()) return bounds();
    AABB bounds;
    for (std::size_t i = 0; i < bodyParts_.size(); ++i) {
        const auto& part = bodyParts_[i];
        if (!part.choices.empty())
            bounds.expand(choiceBounds_[i][(body / (part.base ? part.base : 1)) % part.choices.size()]);
    }
    return bounds;
}

} // namespace apex::editor
