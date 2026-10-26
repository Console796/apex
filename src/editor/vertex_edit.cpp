#include "editor/vertex_edit.hpp"

#include "geometry/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace apex::editor {
namespace {
struct WeldedPoint {
    DVec3 point{0.0};
    std::vector<std::size_t> sources;
};
struct Point2 { double x, y; };

double cross(Point2 o, Point2 a, Point2 b) {
    return (a.x-o.x)*(b.y-o.y) - (a.y-o.y)*(b.x-o.x);
}
double supportArea(const geometry::Face& face, std::span<const DVec3> moved,
                   const geometry::Face& oldFace) {
    const DVec3 normal = face.plane.normal;
    const DVec3 axis = std::abs(normal.x) < 0.8 ? DVec3{1,0,0} : DVec3{0,1,0};
    const DVec3 u = glm::normalize(glm::cross(normal, axis));
    const DVec3 v = glm::cross(normal, u);
    std::vector<Point2> points;
    for (const auto index : oldFace.vertices) {
        const auto point = moved[index];
        if (std::abs(face.plane.signedDistance(point)) > geometry::kPlanarEps) continue;
        points.push_back({glm::dot(point, u), glm::dot(point, v)});
    }
    if (points.size() < 3) return 0.0;
    std::ranges::sort(points, [](Point2 a, Point2 b) {
        return std::tie(a.x, a.y) < std::tie(b.x, b.y);
    });
    points.erase(std::unique(points.begin(), points.end(), [](Point2 a, Point2 b) {
        return std::abs(a.x-b.x) <= geometry::kPointMergeEps &&
               std::abs(a.y-b.y) <= geometry::kPointMergeEps;
    }), points.end());
    if (points.size() < 3) return 0.0;
    std::vector<Point2> hull;
    for (const auto p : points) {
        while (hull.size() >= 2 && cross(hull[hull.size()-2], hull.back(), p) <= 1e-12) hull.pop_back();
        hull.push_back(p);
    }
    const auto lower = hull.size();
    for (auto p = points.rbegin()+1; p != points.rend(); ++p) {
        while (hull.size() > lower && cross(hull[hull.size()-2], hull.back(), *p) <= 1e-12) hull.pop_back();
        hull.push_back(*p);
    }
    if (hull.size() < 4) return 0.0;
    double twiceArea = 0.0;
    for (std::size_t i = 0; i + 1 < hull.size(); ++i)
        twiceArea += hull[i].x*hull[i+1].y - hull[i+1].x*hull[i].y;
    twiceArea += hull.back().x*hull.front().y - hull.front().x*hull.back().y;
    return std::abs(twiceArea) * 0.5;
}
}

Result<VertexEditResult> evaluateVertexEdit(const geometry::ConvexPolyhedron& source,
                                            std::span<const std::size_t> selected,
                                            const Transform& transform) {
    if (!source.isValid()) return std::unexpected(Error{ErrorCode::InvalidArgument, "Source solid geometry is invalid"});
    std::vector<bool> movedVertex(source.vertices.size(), false);
    for (const auto index : selected) {
        if (index >= source.vertices.size())
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Selected vertex is out of range"});
        movedVertex[index] = true;
    }
    std::vector<DVec3> moved;
    moved.reserve(source.vertices.size());
    for (std::size_t i = 0; i < source.vertices.size(); ++i)
        moved.push_back(movedVertex[i] ? transform.point(source.vertices[i]) : source.vertices[i]);
    if (std::ranges::any_of(moved, [](DVec3 p) {
            return !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z);
        })) return std::unexpected(Error{ErrorCode::InvalidArgument, "Move produced a non-finite point"});

    // Match convexHull's first-representative weld and retain its complete provenance.
    std::vector<WeldedPoint> welded;
    for (std::size_t i = 0; i < moved.size(); ++i) {
        const auto found = std::ranges::find_if(welded, [&](const auto& point) {
            return glm::length(point.point - moved[i]) <= geometry::kPointMergeEps;
        });
        if (found == welded.end()) welded.push_back({moved[i], {i}});
        else found->sources.push_back(i);
    }
    std::vector<DVec3> points;
    points.reserve(welded.size());
    for (const auto& point : welded) points.push_back(point.point);
    auto hull = geometry::convexHull(points);
    if (!hull) {
        const auto message = hull.error().empty() ? "Move collapses the solid" : hull.error().front().message;
        return std::unexpected(Error{ErrorCode::InvalidArgument, message});
    }

    VertexEditResult result;
    result.poly = std::move(*hull);
    std::vector<bool> surviving(welded.size(), false);
    result.vertexSources.reserve(result.poly.vertices.size());
    for (const auto point : result.poly.vertices) {
        const auto found = std::ranges::find_if(welded, [&](const auto& candidate) {
            return glm::length(candidate.point - point) <= geometry::kPointMergeEps;
        });
        if (found == welded.end())
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Hull vertex lost its source provenance"});
        const auto index = static_cast<std::size_t>(found - welded.begin());
        surviving[index] = true;
        result.vertexSources.push_back(found->sources);
        if (found->sources.size() > 1) result.mergedVertices.push_back(found->sources);
    }
    for (std::size_t i = 0; i < welded.size(); ++i)
        if (!surviving[i]) result.absorbedVertices.insert(result.absorbedVertices.end(),
                                                           welded[i].sources.begin(), welded[i].sources.end());
    std::ranges::sort(result.absorbedVertices);

    for (std::size_t n = 0; n < result.poly.faces.size(); ++n) {
        auto& face = result.poly.faces[n];
        std::optional<std::size_t> donor;
        double bestArea = 0.0;
        std::size_t bestCorners = 0;
        double bestAlignment = -2.0;
        for (std::size_t o = 0; o < source.faces.size(); ++o) {
            const auto area = supportArea(face, moved, source.faces[o]);
            if (area <= geometry::kMinFaceArea) continue;
            std::size_t corners = 0;
            for (const auto vertex : face.vertices) {
                const auto& origins = result.vertexSources[vertex];
                if (std::ranges::any_of(origins, [&](auto old) {
                        return std::ranges::find(source.faces[o].vertices, old) != source.faces[o].vertices.end();
                    })) ++corners;
            }
            const double alignment = glm::dot(face.plane.normal, source.faces[o].plane.normal);
            if (!donor || area > bestArea + geometry::kMinFaceArea ||
                (std::abs(area-bestArea) <= geometry::kMinFaceArea &&
                 (corners > bestCorners || (corners == bestCorners &&
                  (alignment > bestAlignment || (alignment == bestAlignment &&
                   source.faces[o].sideId < source.faces[*donor].sideId)))))) {
                donor = o;
                bestArea = area;
                bestCorners = corners;
                bestAlignment = alignment;
            }
        }
        if (donor) {
            static_cast<geometry::FaceAttributes&>(face) = source.faces[*donor];
            result.correspondence->emplace(n, source.faces[*donor].sideId);
        } else {
            const auto donorFace = std::ranges::max_element(source.faces, {}, [&](const auto& old) {
                return glm::dot(face.plane.normal, old.plane.normal);
            });
            if (donorFace != source.faces.end()) static_cast<geometry::FaceAttributes&>(face) = *donorFace;
        }
    }
    // Use the writer's largest-area child ownership rule for split markers.
    std::map<document::SideId, std::size_t> owner;
    for (const auto& [faceIndex, side] : *result.correspondence) {
        const auto [it, inserted] = owner.emplace(side, faceIndex);
        if (!inserted && result.poly.faceArea(result.poly.faces[faceIndex]) >
                         result.poly.faceArea(result.poly.faces[it->second])) it->second = faceIndex;
    }
    for (const auto& [faceIndex, side] : *result.correspondence)
        if (owner.at(side) != faceIndex) result.splitFaces.push_back(faceIndex);
    return result;
}

} // namespace apex::editor
