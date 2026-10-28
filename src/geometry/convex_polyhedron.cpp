#include "geometry/convex_polyhedron.hpp"

#include "geometry/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace apex::geometry {
namespace {

bool finite(DVec3 point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

bool validPlane(const Plane& plane) {
    return finite(plane.normal) && std::isfinite(plane.dist) &&
           std::abs(glm::length(plane.normal) - 1.0) <= kNormalMatchEps;
}

bool near(DVec3 a, DVec3 b) {
    return glm::length(a - b) <= kPointMergeEps;
}

std::optional<std::array<DVec3, 3>> largestTriangle(std::span<const DVec3> points) {
    double largest = 0.0;
    std::optional<std::array<DVec3, 3>> result;
    for (const auto point : points) {
        if (!finite(point)) {
            return std::nullopt;
        }
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (std::size_t j = i + 1; j < points.size(); ++j) {
            for (std::size_t k = j + 1; k < points.size(); ++k) {
                const double area = glm::length(glm::cross(points[j] - points[i],
                                                          points[k] - points[i])) * 0.5;
                if (std::isfinite(area) && area > largest && area >= kMinFaceArea) {
                    largest = area;
                    result = {{points[i], points[j], points[k]}};
                }
            }
        }
    }
    return result;
}

DVec3 areaVector(std::span<const DVec3> points) {
    DVec3 result{0.0};
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        result += glm::cross(points[i] - points[0], points[i + 1] - points[0]) * 0.5;
    }
    return result;
}

std::vector<DVec3> facePoints(const ConvexPolyhedron& polyhedron, const Face& face) {
    std::vector<DVec3> points;
    for (const auto index : face.vertices) {
        if (index >= polyhedron.vertices.size()) {
            return {};
        }
        points.push_back(polyhedron.vertices[index]);
    }
    return points;
}

struct Polygon {
    PlaneSide side;
    std::vector<DVec3> points;
    std::optional<std::size_t> sourceIndex; // Absent for the six seed-box faces.
    bool isCap = false;
    std::optional<std::size_t> carverFace = std::nullopt;
};

void appendDistinct(std::vector<DVec3>& points, DVec3 point) {
    if (points.empty() || !near(points.back(), point)) {
        points.push_back(point);
    }
}

void closeLoop(std::vector<DVec3>& points) {
    if (points.size() > 1 && near(points.front(), points.back())) {
        points.pop_back();
    }
}

bool substantial(const Polygon& polygon) {
    return polygon.points.size() >= 3 &&
           glm::length(areaVector(polygon.points)) >= kMinFaceArea;
}

std::vector<Polygon> seedBox() {
    constexpr double halfSize = 524288.0; // Side length 2^20, centered at the origin.
    const std::array<DVec3, 8> points{{{-halfSize, -halfSize, -halfSize},
                                      {halfSize, -halfSize, -halfSize},
                                      {halfSize, halfSize, -halfSize},
                                      {-halfSize, halfSize, -halfSize},
                                      {-halfSize, -halfSize, halfSize},
                                      {halfSize, -halfSize, halfSize},
                                      {halfSize, halfSize, halfSize},
                                      {-halfSize, halfSize, halfSize}}};
    const std::array<std::array<std::size_t, 4>, 6> loops{{{0, 3, 2, 1}, {4, 5, 6, 7},
                                                         {0, 4, 7, 3}, {1, 2, 6, 5},
                                                         {0, 1, 5, 4}, {3, 7, 6, 2}}};
    std::vector<Polygon> result;
    for (const auto& loop : loops) {
        Polygon polygon;
        for (const auto index : loop) {
            polygon.points.push_back(points[index]);
        }
        polygon.side.plane = Plane::fromPoints(polygon.points[0], polygon.points[1],
                                              polygon.points[2]);
        result.push_back(std::move(polygon));
    }
    return result;
}

std::vector<Polygon> clipPolygons(std::vector<Polygon> polygons, const PlaneSide& side,
                                  std::size_t sideIndex, bool isCap = false) {
    std::vector<Polygon> clipped;
    std::vector<DVec3> cap;
    const auto addCap = [&cap](DVec3 point) {
        if (std::none_of(cap.begin(), cap.end(), [point](DVec3 p) { return near(p, point); })) {
            cap.push_back(point);
        }
    };
    for (auto& polygon : polygons) {
        std::vector<DVec3> loop;
        for (std::size_t i = 0; i < polygon.points.size(); ++i) {
            const auto a = polygon.points[i];
            const auto b = polygon.points[(i + 1) % polygon.points.size()];
            double da = side.plane.signedDistance(a);
            double db = side.plane.signedDistance(b);
            // Classify near-plane endpoints as on the boundary without projecting them.
            if (std::abs(da) <= kPlanarEps) {
                da = 0.0;
            }
            if (std::abs(db) <= kPlanarEps) {
                db = 0.0;
            }
            if (da <= 0.0) {
                appendDistinct(loop, a);
                if (da == 0.0) {
                    addCap(a);
                }
            }
            if ((da < 0.0 && db > 0.0) || (da > 0.0 && db < 0.0)) {
                const auto point = a + (b - a) * (da / (da - db));
                appendDistinct(loop, point);
                addCap(point);
            }
        }
        closeLoop(loop);
        polygon.points = std::move(loop);
        if (substantial(polygon)) {
            clipped.push_back(std::move(polygon));
        }
    }
    if (cap.size() >= 3) {
        DVec3 midpoint{0.0};
        for (const auto point : cap) {
            midpoint += point / static_cast<double>(cap.size());
        }
        const auto normal = side.plane.normal;
        const auto reference = std::abs(normal.z) < 0.5 ? DVec3{0.0, 0.0, 1.0}
                                                       : DVec3{1.0, 0.0, 0.0};
        const auto u = glm::normalize(glm::cross(reference, normal));
        const auto v = glm::cross(normal, u);
        std::stable_sort(cap.begin(), cap.end(), [=](DVec3 a, DVec3 b) {
            const auto pa = a - midpoint;
            const auto pb = b - midpoint;
            return std::atan2(glm::dot(pa, v), glm::dot(pa, u)) <
                   std::atan2(glm::dot(pb, v), glm::dot(pb, u));
        });
        Polygon polygon{side, std::move(cap), sideIndex, isCap};
        if (substantial(polygon)) {
            clipped.push_back(std::move(polygon));
        }
    }
    return clipped;
}

std::expected<std::vector<Polygon>, std::vector<Problem>>
clipPlanes(std::span<const PlaneSide> sides) {
    std::vector<Problem> problems;
    for (std::size_t i = 0; i < sides.size(); ++i) {
        if (!validPlane(sides[i].plane)) {
            problems.push_back({ProblemCode::InvalidPlane, "Expected a finite unit plane", i});
        }
    }
    if (!problems.empty()) {
        return std::unexpected(std::move(problems));
    }
    auto polygons = seedBox();
    for (std::size_t sideIndex = 0; sideIndex < sides.size(); ++sideIndex) {
        const auto& side = sides[sideIndex];
        bool outside = false;
        bool inside = false;
        for (const auto& polygon : polygons) {
            for (const auto point : polygon.points) {
                const double distance = side.plane.signedDistance(point);
                outside |= distance > kPlanarEps;
                inside |= distance < -kPlanarEps;
            }
        }
        if (!outside) {
            continue; // Redundant, including a duplicate of an existing boundary plane.
        }
        if (!inside) {
            return std::unexpected(std::vector<Problem>{
                {ProblemCode::Empty, "Plane half-spaces have no solid interior", sideIndex}});
        }
        polygons = clipPolygons(std::move(polygons), side, sideIndex);
    }
    if (std::any_of(polygons.begin(), polygons.end(), [](const Polygon& polygon) {
            return !polygon.sourceIndex.has_value();
        })) {
        return std::unexpected(std::vector<Problem>{
            {ProblemCode::Unbounded, "Input reaches the construction box boundary"}});
    }
    return polygons;
}

std::expected<ConvexPolyhedron, std::vector<Problem>> assemble(std::span<const Polygon> polygons) {
    ConvexPolyhedron result;
    std::vector<Problem> problems;
    for (const auto& polygon : polygons) {
        Face face;
        static_cast<FaceAttributes&>(face) = polygon.side;
        face.isCap = polygon.isCap;
        face.carverFace = polygon.carverFace;
        for (const auto point : polygon.points) {
            const auto found = std::find_if(result.vertices.begin(), result.vertices.end(),
                                            [point](DVec3 p) { return near(p, point); });
            const auto index = static_cast<std::size_t>(found - result.vertices.begin());
            if (found == result.vertices.end()) {
                result.vertices.push_back(point);
            }
            face.vertices.push_back(index);
        }
        const auto plane = result.derivePlane(face);
        if (plane) {
            face.plane = *plane;
            if (glm::dot(plane->normal, polygon.side.plane.normal) <= 0.0) {
                problems.push_back({ProblemCode::InvertedWinding,
                                    "Vertex winding opposes the input side plane",
                                    result.faces.size()});
            }
        }
        result.faces.push_back(std::move(face));
    }
    auto validity = result.validate();
    problems.insert(problems.end(), validity.begin(), validity.end());
    if (!problems.empty()) {
        return std::unexpected(std::move(problems));
    }
    return result;
}

void requireDimensions(int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Texture dimensions must be positive");
    }
}

} // namespace

std::optional<Plane> planePointsFromSource(DVec3 a, DVec3 b, DVec3 c) {
    const std::array points{a, b, c};
    if (!largestTriangle(points)) {
        return std::nullopt;
    }
    return Plane::fromPoints(a, c, b);
}

std::expected<ConvexPolyhedron, std::vector<Problem>>
ConvexPolyhedron::fromPlanes(std::span<const PlaneSide> sides) {
    auto polygons = clipPlanes(sides);
    if (!polygons) {
        return std::unexpected(std::move(polygons.error()));
    }
    return assemble(*polygons);
}

std::expected<ConvexPolyhedron, std::vector<Problem>>
ConvexPolyhedron::fromPointData(std::span<const PointDataSide> sides) {
    std::vector<PlaneSide> planes;
    std::vector<bool> trusted;
    for (std::size_t i = 0; i < sides.size(); ++i) {
        const auto& side = sides[i];
        if (!validPlane(side.plane)) {
            return std::unexpected(std::vector<Problem>{
                {ProblemCode::InvalidPlane, "Expected a finite unit plane", i}});
        }
        planes.push_back(side);
        trusted.push_back(!side.points.empty() &&
                          std::all_of(side.points.begin(), side.points.end(), [&](DVec3 point) {
                              return finite(point) && std::abs(side.plane.signedDistance(point)) <=
                                                          kPointDataTrustEps;
                          }));
    }
    std::vector<Polygon> fallback;
    if (std::find(trusted.begin(), trusted.end(), false) != trusted.end()) {
        auto clipped = clipPlanes(planes);
        if (!clipped) {
            return std::unexpected(std::move(clipped.error()));
        }
        fallback = std::move(*clipped);
    }
    std::vector<Polygon> polygons;
    for (std::size_t i = 0; i < sides.size(); ++i) {
        if (trusted[i]) {
            polygons.push_back({planes[i], sides[i].points, i});
        } else {
            const auto found = std::find_if(fallback.begin(), fallback.end(), [i](const auto& p) {
                return p.sourceIndex == i;
            });
            if (found != fallback.end()) {
                polygons.push_back(*found);
            }
        }
    }
    return assemble(polygons);
}

std::optional<Plane> ConvexPolyhedron::derivePlane(const Face& face) const {
    const auto triangle = largestTriangle(facePoints(*this, face));
    if (!triangle) {
        return std::nullopt;
    }
    return Plane::fromPoints((*triangle)[0], (*triangle)[1], (*triangle)[2]);
}

std::optional<std::array<DVec3, 3>>
ConvexPolyhedron::sourcePlanePoints(const Face& face) const {
    auto triangle = largestTriangle(facePoints(*this, face));
    if (triangle) {
        std::swap((*triangle)[1], (*triangle)[2]);
    }
    return triangle;
}

std::vector<Problem> ConvexPolyhedron::validate() const {
    std::vector<Problem> problems;
    if (vertices.size() < 4 || faces.size() < 4) {
        problems.push_back({ProblemCode::Empty, "A solid needs at least four vertices and faces"});
    }
    if (std::any_of(vertices.begin(), vertices.end(), [](DVec3 p) { return !finite(p); })) {
        problems.push_back({ProblemCode::InvalidVertex, "Nonfinite vertex coordinate"});
        return problems;
    }
    struct EdgeUse {
        std::size_t count = 0;
        int orientation = 0;
    };
    std::map<std::pair<std::size_t, std::size_t>, EdgeUse> edges;
    std::set<std::size_t> usedVertices;
    const auto origin = center();
    double volume = 0.0;
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const auto& face = faces[f];
        const auto add = [&](ProblemCode code, const char* message) {
            problems.push_back({code, message, f});
        };
        if (std::any_of(face.vertices.begin(), face.vertices.end(), [&](std::size_t index) {
                return index >= vertices.size();
            })) {
            add(ProblemCode::InvalidIndex, "Face references a missing vertex");
            continue;
        }
        const auto points = facePoints(*this, face);
        const auto derived = derivePlane(face);
        if (!derived) {
            add(ProblemCode::DegenerateFace, "Face has no triangle of sufficient area");
            continue;
        }
        if (glm::length(areaVector(points)) < kMinFaceArea) {
            add(ProblemCode::DegenerateFace, "Face has insufficient area");
        }
        // Use a local origin to avoid cancellation near the Source coordinate limits.
        volume += glm::dot(points[0] - origin, areaVector(points)) / 3.0;
        if (!validPlane(face.plane)) {
            add(ProblemCode::InvalidPlane, "Face plane is not finite and normalized");
        } else {
            if (glm::dot(face.plane.normal, derived->normal) <= 0.0) {
                add(ProblemCode::InvertedWinding, "Loop opposes the cached outward normal");
            } else if (glm::length(face.plane.normal - derived->normal) > kNormalMatchEps) {
                add(ProblemCode::InvalidPlane, "Cached normal disagrees with vertices");
            }
        }
        for (const auto point : points) {
            if (std::abs(derived->signedDistance(point)) > kPlanarEps ||
                (validPlane(face.plane) &&
                 std::abs(face.plane.signedDistance(point)) > kPlanarEps)) {
                add(ProblemCode::NonPlanarFace, "Face vertices disagree with its plane");
                break;
            }
        }
        std::set<std::size_t> loopVertices;
        for (std::size_t i = 0; i < face.vertices.size(); ++i) {
            const auto a = face.vertices[i];
            const auto b = face.vertices[(i + 1) % face.vertices.size()];
            usedVertices.insert(a);
            if (!loopVertices.insert(a).second) {
                add(ProblemCode::DegenerateFace, "Repeated vertex in face loop");
            }
            auto& edge = edges[std::minmax(a, b)];
            ++edge.count;
            edge.orientation += a < b ? 1 : -1;
            const auto delta = vertices[b] - vertices[a];
            const double length = glm::length(delta);
            if (length < kMinEdgeLength) {
                add(ProblemCode::ShortEdge, "Edge is below minimum length");
            }
            for (const auto point : points) {
                if (glm::dot(glm::cross(delta, point - vertices[a]), derived->normal) <
                    -kPlanarEps * length) {
                    add(ProblemCode::NonConvex, "Face loop is not convex and counter-clockwise");
                    break;
                }
            }
        }
        if (std::any_of(vertices.begin(), vertices.end(), [&](DVec3 point) {
                return derived->signedDistance(point) > kPlanarEps;
            })) {
            add(ProblemCode::NonConvex, "Vertices extend outside an outward face plane");
        }
    }
    for (const auto& [edge, use] : edges) {
        (void)edge;
        if (use.count != 2) {
            problems.push_back({ProblemCode::OpenEdge, "Edge must be shared by exactly two faces"});
        } else if (use.orientation != 0) {
            problems.push_back({ProblemCode::InvertedWinding,
                                "Adjacent faces traverse a shared edge in the same direction"});
        }
    }
    if (usedVertices.size() != vertices.size()) {
        problems.push_back({ProblemCode::InvalidVertex, "Polyhedron contains unused vertices"});
    }
    // V - E + F = 2, written without unsigned subtraction.
    if (usedVertices.size() + faces.size() != edges.size() + 2) {
        problems.push_back({ProblemCode::EulerCharacteristic, "Expected V - E + F = 2"});
    }
    if (!std::isfinite(volume) || volume <= kMinFaceArea * kMinEdgeLength / 3.0) {
        problems.push_back({ProblemCode::DegenerateVolume, "Solid has no positive volume"});
    }
    return problems;
}

std::vector<std::array<std::size_t, 3>> ConvexPolyhedron::triangulate(const Face& face) const {
    std::vector<std::array<std::size_t, 3>> result;
    for (std::size_t i = 1; i + 1 < face.vertices.size(); ++i) {
        result.push_back({face.vertices[0], face.vertices[i], face.vertices[i + 1]});
    }
    return result;
}

double ConvexPolyhedron::faceArea(const Face& face) const {
    return glm::length(areaVector(facePoints(*this, face)));
}

double ConvexPolyhedron::volume() const {
    if (vertices.empty()) return 0.0;
    // Tetrahedra from a vertex: the bounds center is far from a sliver's faces and loses precision.
    const auto origin = vertices.front();
    double result = 0.0;
    for (const auto& face : faces) {
        // Keep the same invalid-index behavior as facePoints without allocating a loop.
        if (face.vertices.empty() || std::ranges::any_of(face.vertices,
                [&](auto index) { return index >= vertices.size(); })) continue;
        const auto first = vertices[face.vertices[0]] - origin;
        for (std::size_t i = 1; i + 1 < face.vertices.size(); ++i)
            result += glm::dot(first, glm::cross(vertices[face.vertices[i]] - origin,
                                               vertices[face.vertices[i + 1]] - origin)) / 6.0;
    }
    return result;
}

namespace {
struct PlaneClassification { bool front = false, back = false; };

PlaneClassification classify(const ConvexPolyhedron& poly, const Plane& plane) {
    PlaneClassification result;
    for (const auto point : poly.vertices) {
        const double distance = plane.signedDistance(point);
        result.front |= distance > kPlanarEps;
        result.back |= distance < -kPlanarEps;
        if (result.front && result.back) break;
    }
    return result;
}

// The caller has validated the solid and plane and found vertices on both sides.
SplitResult splitCrossing(const ConvexPolyhedron& poly, const Plane& plane) {
    SplitResult result;
    const auto piece = [&](Plane boundary) {
        std::vector<Polygon> polygons;
        polygons.reserve(poly.faces.size());
        for (std::size_t i = 0; i < poly.faces.size(); ++i) {
            const auto& face = poly.faces[i];
            PlaneSide side;
            static_cast<FaceAttributes&>(side) = face;
            side.plane = face.plane;
            polygons.push_back({side, facePoints(poly, face), i, face.isCap, face.carverFace});
        }
        PlaneSide cap;
        cap.plane = boundary;
        auto clipped = clipPolygons(std::move(polygons), cap, poly.faces.size(), true);
        auto assembled = assemble(clipped);
        if (!assembled) throw std::invalid_argument("Split: " + assembled.error().front().message);
        return std::move(*assembled);
    };
    result.back = piece(plane);
    result.front = piece({-plane.normal, -plane.dist});
    result.backCap = result.back->faces.size() - 1;
    result.frontCap = result.front->faces.size() - 1;
    return result;
}
} // namespace

SplitResult split(const ConvexPolyhedron& poly, const Plane& plane) {
    if (!validPlane(plane) || !poly.isValid())
        throw std::invalid_argument("Split requires a valid solid and finite unit plane");
    const auto sides = classify(poly, plane);
    SplitResult result;
    if (!sides.front) { result.back = poly; return result; }
    if (!sides.back) { result.front = poly; return result; }
    return splitCrossing(poly, plane);
}

std::vector<ConvexPolyhedron> subtract(const ConvexPolyhedron& target,
                                       const ConvexPolyhedron& carver) {
    if (!target.isValid() || !carver.isValid())
        throw std::invalid_argument("Subtract requires valid solids");
    if (!target.bounds().intersects(carver.bounds())) return {target};
    // Reject separating planes before building fragments that would be discarded.
    for (const auto& face : carver.faces) {
        const auto sides = classify(target, face.plane);
        if (sides.front && !sides.back) return {target};
    }
    std::vector<ConvexPolyhedron> fragments;
    fragments.reserve(carver.faces.size());
    std::optional<ConvexPolyhedron> remainder = target;
    for (std::size_t i = 0; i < carver.faces.size() && remainder; ++i) {
        const auto& plane = carver.faces[i].plane;
        const auto sides = classify(*remainder, plane);
        if (!sides.front) continue;
        if (!sides.back) return {target};
        // Inputs are validated above; splitCrossing validates both assembled outputs.
        auto cut = splitCrossing(*remainder, plane);
        if (cut.frontCap) cut.front->faces[*cut.frontCap].carverFace = i;
        if (cut.backCap) cut.back->faces[*cut.backCap].carverFace = i;
        if (cut.front) fragments.push_back(std::move(*cut.front));
        remainder = std::move(cut.back);
    }
    // Earlier planes can cut an entirely disjoint target into several temporary pieces.
    if (!remainder) return {target};
    return fragments;
}

AABB ConvexPolyhedron::bounds() const {
    AABB result;
    for (const auto point : vertices) {
        result.expand(point);
    }
    return result;
}

DVec3 ConvexPolyhedron::center() const {
    return bounds().center();
}

FaceUv generateUv(const Face& face, int textureWidth, int textureHeight) {
    requireDimensions(textureWidth, textureHeight);
    if (!validPlane(face.plane)) {
        throw std::invalid_argument("UV generation requires a finite unit face plane");
    }
    FaceUv result;
    const auto normal = glm::abs(face.plane.normal);
    if (normal.z >= normal.x && normal.z >= normal.y) {
        result.uaxis = {1.0, 0.0, 0.0};
        result.vaxis = {0.0, -1.0, 0.0};
    } else if (normal.x >= normal.y) {
        result.uaxis = {0.0, 1.0, 0.0};
        result.vaxis = {0.0, 0.0, -1.0};
    } else {
        result.uaxis = {1.0, 0.0, 0.0};
        result.vaxis = {0.0, 0.0, -1.0};
    }
    return result;
}

DVec2 uvAt(const Face& face, DVec3 point, int textureWidth, int textureHeight) {
    requireDimensions(textureWidth, textureHeight);
    const auto& uv = face.uv;
    if (!finite(point) || !finite(uv.uaxis) || !finite(uv.vaxis) ||
        !std::isfinite(uv.shiftU) || !std::isfinite(uv.shiftV) ||
        !std::isfinite(uv.scaleU) || !std::isfinite(uv.scaleV) ||
        uv.scaleU == 0.0 || uv.scaleV == 0.0) {
        throw std::invalid_argument("UV evaluation requires finite data and nonzero scales");
    }
    const double width = static_cast<double>(textureWidth);
    const double height = static_cast<double>(textureHeight);
    return {glm::dot(point, uv.uaxis) / (width * uv.scaleU) + uv.shiftU / width,
            glm::dot(point, uv.vaxis) / (height * uv.scaleV) + uv.shiftV / height};
}

} // namespace apex::geometry
