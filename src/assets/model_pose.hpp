#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace mdlpp::MDL {
struct MDL;
}

namespace apex::assets {

// Row-major 3x4: three rows of {x axis, y axis, z axis, translation} acting on column vectors.
using BoneMatrix = std::array<float, 12>;

// Hammer and the engine show a model in sequence 0's first animation at frame 0, and studiomdl computes the
// hull and bounds from that pose. The vertices in the VVD are in reference space, so a model whose first
// sequence moves its bones (typically a root bone turned by the upaxis conversion) is drawn rotated unless
// that pose is applied.
//
// Returns one skinning matrix (bone pose * poseToBone) per bone. Empty when the animation is unavailable
// (external .ani blocks, delta animations, malformed data) or leaves every bone at the reference pose.
// `mdlBytes` is the raw .mdl file: mdlpp flattens the per-axis animation values, which loses which axis each
// value belongs to.
std::vector<BoneMatrix> sequenceZeroSkinning(const mdlpp::MDL::MDL& mdl, std::span<const std::byte> mdlBytes);

} // namespace apex::assets
