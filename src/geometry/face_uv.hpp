#pragma once

#include "geometry/convex_polyhedron.hpp"

namespace apex::geometry {

enum class Justify { Left, Right, Top, Bottom, Center, Fit };
enum class HorizontalAlignment { Left, Center, Right, None };
enum class VerticalAlignment { Top, Center, Bottom, None };
struct Alignment { HorizontalAlignment horizontal = HorizontalAlignment::None; VerticalAlignment vertical = VerticalAlignment::None; };
struct FacePlaneFrame { DVec3 origin{}, x{1, 0, 0}, y{0, 1, 0}; };
// Canonical tile convention: left/top bound = 0, right/bottom = w/h, centre = w/2,h/2.
// Fit preserves scale signs, spans one tile, then puts the minima at zero. Empty or
// zero-span fit bounds are invalid. Points may be the union of several face loops. Justify and wrap
// keep the shifts they compute within one texture (|shift| < w or h); whole textures look the same.
FaceUv justify(FaceUv uv, std::span<const DVec3> points, Justify mode, int w, int h);
FaceUv justify2(FaceUv uv, std::span<const DVec3> points, HorizontalAlignment horizontal,
                VerticalAlignment vertical, int w, int h);
Alignment alignmentOf(const FaceUv& uv, std::span<const DVec3> points, int w, int h);
// `du` and `dv` are texels; dimensions are needed to reduce equivalent tile offsets.
FaceUv translateUv(FaceUv uv, double du, double dv, int w = 256, int h = 256);
FaceUv alignWorld(FaceUv uv, DVec3 normal);
FaceUv alignFace(FaceUv uv, DVec3 normal);
// Copy a source projection unchanged when neither axis is degenerate on the target plane.
// A null result means an axis is within two degrees of the target normal.
std::optional<FaceUv> projectUv(const FaceUv& source, DVec3 targetNormal);
// Align to the target face, then apply the source's derived world-rule rotation and mapping values.
FaceUv matchUv(const FaceUv& source, DVec3 sourceNormal, DVec3 targetNormal);
// Positive rotation is counter-clockwise viewed from outside (right hand about normal).
// Rotation is derived from projected axes; the stored VMF rotation is never an input.
FaceUv rotateUv(FaceUv uv, DVec3 normal, double degrees);
FaceUv rotateUvAbout(FaceUv uv, DVec3 normal, DVec3 pivot, double degrees);
FaceUv scaleUvAbout(FaceUv uv, DVec3 pivot, double factorU, double factorV);
FacePlaneFrame planeFrame(DVec3 normal, std::span<const DVec3> points);
double deriveRotation(const FaceUv& uv, DVec3 normal);
// Normals must be finite unit vectors; edge must be nondegenerate and parallel to both planes.
FaceUv wrapUv(const FaceUv& from, DVec3 fromNormal, DVec3 toNormal,
              DVec3 edgeA, DVec3 edgeB, int w, int h);

} // namespace apex::geometry
