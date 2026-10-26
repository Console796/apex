#pragma once

#include "geometry/convex_polyhedron.hpp"

#include <cstdint>
#include <map>
#include <optional>

namespace apex::geometry {

// Absent means plane matching; a supplied map identifies surviving faces by old side ID.
// Unlisted faces are new. If children share a parent ID, the largest keeps its identity
// (ties prefer the lower new index); the others inherit from that parent.
using FaceCorrespondence = std::optional<std::map<std::size_t, std::uint32_t>>;

struct FaceMatch {
    std::size_t newIndex;
    std::size_t oldIndex;
};

struct NewFace {
    std::size_t newIndex;
    std::optional<std::size_t> inheritFrom; // Empty only when there are no old faces.
};

struct MatchResult {
    std::vector<FaceMatch> matched;
    std::vector<NewFace> unmatchedNew;
    std::vector<std::size_t> vanishedOld;
};

// Deterministic one-to-one matching of outward derived planes, visiting new faces in order.
// Prefer free candidates by normal difference, plane distance, then old index; augment
// previous assignments when necessary to maximize matches, including coplanar duplicates.
// Copies every FaceAttributes field on matches. Unmatched faces keep their input attributes:
// the caller allocates fresh IDs and copies texture data from inheritFrom. Geometry is unchanged.
MatchResult matchFaces(const ConvexPolyhedron& oldPolyhedron, ConvexPolyhedron& newPolyhedron);

} // namespace apex::geometry
