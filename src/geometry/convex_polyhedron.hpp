#pragma once

#include "core/math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::geometry {

struct FaceUv {
    DVec3 uaxis{1.0, 0.0, 0.0};
    DVec3 vaxis{0.0, -1.0, 0.0};
    double shiftU = 0.0;
    double shiftV = 0.0;
    double scaleU = 1.0;
    double scaleV = 1.0;
};

struct FaceAttributes {
    std::uint32_t sideId = 0; // Identity allocation belongs to the caller.
    std::string material;
    FaceUv uv;
    int lightmapScale = 16;
    std::uint32_t smoothingGroups = 0;
};

struct Face : FaceAttributes {
    // Convex loops, counter-clockwise as seen from OUTSIDE the solid. Collinear edge
    // subdivisions are allowed. Plane points in Source files use the opposite winding.
    std::vector<std::size_t> vertices;
    Plane plane; // Derived cache; refresh with derivePlane after editing vertices.
    bool isCap = false;
    std::optional<std::size_t> carverFace; // Index in the carver that created this cap.
};

struct PlaneSide : FaceAttributes {
    Plane plane; // Unit outward normal; the solid lies in dot(normal, p) <= dist.
};

struct PointDataSide : PlaneSide {
    // Same counter-clockwise convention as Face, not Source's plane-point convention.
    // Empty means absent. Trusted malformed loops are rejected, never silently repaired.
    std::vector<DVec3> points;
};

enum class ProblemCode {
    Empty,
    InvalidPlane,
    InvalidVertex,
    InvalidIndex,
    DegenerateFace,
    DegenerateVolume,
    ShortEdge,
    NonPlanarFace,
    InvertedWinding,
    NonConvex,
    OpenEdge,
    EulerCharacteristic,
    Unbounded
};

struct Problem {
    ProblemCode code;
    std::string message;
    std::optional<std::size_t> faceIndex = std::nullopt;
};

using Problems = std::vector<Problem>;

// Source's three plane points are clockwise when viewed from outside. Reverse the cross
// product here (and only here), so the resulting normal points OUT. Core Plane::fromPoints
// uses the usual right-hand, counter-clockwise convention. Collinear/nonfinite input fails.
std::optional<Plane> planePointsFromSource(DVec3 a, DVec3 b, DVec3 c);

class ConvexPolyhedron {
public:
    std::vector<DVec3> vertices;
    std::vector<Face> faces;

    // Clip a centered box of SIDE LENGTH 2^20 units. A surviving seed face means the
    // input is unbounded (or exceeds that construction domain) and is rejected.
    // Redundant/coplanar sides are discarded; the first coincident side keeps its data.
    static std::expected<ConvexPolyhedron, std::vector<Problem>>
    fromPlanes(std::span<const PlaneSide> sides);

    // Reconcile per side (D8): trust nonempty points iff all lie within kPointDataTrustEps
    // of its input plane; otherwise use that side's plane-intersection polygon. Trusted
    // coordinates are not projected. Welding shares the first representative within
    // kPointMergeEps. Invalid trusted topology/planarity is returned as problems.
    static std::expected<ConvexPolyhedron, std::vector<Problem>>
    fromPointData(std::span<const PointDataSide> sides);

    std::vector<Problem> validate() const;
    bool isValid() const { return validate().empty(); }

    // Largest triangle among the face's own vertices, with loop orientation preserved.
    // Invalid indices, nonfinite vertices or a degenerate face return nullopt.
    std::optional<Plane> derivePlane(const Face& face) const;
    std::optional<std::array<DVec3, 3>> sourcePlanePoints(const Face& face) const;

    // Fan triangulation is valid because valid faces are convex. Indices address vertices.
    std::vector<std::array<std::size_t, 3>> triangulate(const Face& face) const;
    double faceArea(const Face& face) const;
    double volume() const; // Positive for a valid outward-wound solid; local origin.
    AABB bounds() const;
    DVec3 center() const; // Center of the AABB; zero for an empty polyhedron.
};

std::expected<ConvexPolyhedron, Problems> convexHull(std::span<const DVec3> points);

struct SplitResult {
    std::optional<ConvexPolyhedron> front, back;
    std::optional<std::size_t> frontCap, backCap; // The new cap in each piece, if cut.
};

// Front has positive signed distance. Surviving faces retain attributes and provenance.
// Invalid inputs or cuts below the kernel's validity tolerances throw invalid_argument.
SplitResult split(const ConvexPolyhedron& poly, const Plane& plane);
// Sequential outside fragments, in carver face order. No intersection returns target verbatim.
std::vector<ConvexPolyhedron> subtract(const ConvexPolyhedron& target,
                                       const ConvexPolyhedron& carver);

// Hammer's world-aligned dominant-normal projection: horizontal faces use +X/-Y;
// X-facing walls use +Y/-Z; Y-facing walls use +X/-Z. Ties prefer Z, then X, then Y.
// Default shifts are zero and scales are one; dimensions do not change these world axes.
// UV functions throw std::invalid_argument for nonpositive dimensions or invalid UV data.
FaceUv generateUv(const Face& face, int textureWidth, int textureHeight);
DVec2 uvAt(const Face& face, DVec3 point, int textureWidth, int textureHeight);

} // namespace apex::geometry
