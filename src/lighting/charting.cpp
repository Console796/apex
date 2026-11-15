#include "lighting/charting.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace apex::lighting {
namespace {

bool finite(DVec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct Bounds {
    DVec2 mins{std::numeric_limits<double>::infinity()};
    DVec2 maxs{-std::numeric_limits<double>::infinity()};
};

Bounds bounds(std::span<const DVec3> vertices, const LuxelChart& frame) {
    Bounds result;
    for (const auto p : vertices) {
        const DVec2 st{glm::dot(p, frame.worldToLuxel[0]), glm::dot(p, frame.worldToLuxel[1])};
        result.mins = glm::min(result.mins, st);
        result.maxs = glm::max(result.maxs, st);
    }
    return result;
}

// Inverse of the frame on the face plane: luxel steps and the world position of local sample (0, 0).
void solveInverse(LuxelChart& frame, const Plane& plane) {
    const auto cross = glm::cross(frame.worldToLuxel[0], frame.worldToLuxel[1]);
    const double det = glm::dot(cross, plane.normal);
    frame.luxelToWorld[0] = glm::cross(frame.worldToLuxel[1], plane.normal) / det;
    frame.luxelToWorld[1] = glm::cross(plane.normal, frame.worldToLuxel[0]) / det;
    frame.origin = cross * (plane.dist / det) +
                   frame.luxelToWorld[0] * (frame.mins[0] - frame.offset.x) +
                   frame.luxelToWorld[1] * (frame.mins[1] - frame.offset.y);
}

LuxelChart chart(std::span<const DVec3> vertices, LuxelChart frame, const Plane& plane) {
    const auto b = bounds(vertices, frame);
    for (int i = 0; i < 2; ++i) {
        const double lo = std::floor(b.mins[i] + frame.offset[i]);
        const double hi = std::ceil(b.maxs[i] + frame.offset[i]);
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo < std::numeric_limits<int>::min() ||
            hi > std::numeric_limits<int>::max() || hi - lo >= std::numeric_limits<int>::max())
            throw std::invalid_argument("lightmap extents out of range");
        frame.mins[i] = static_cast<int>(lo);
        frame.extents[i] = static_cast<int>(hi - lo);
        frame.dimensions[i] = frame.extents[i] + 1;
    }
    solveInverse(frame, plane);
    if (!finite(frame.origin) || !finite(frame.luxelToWorld[0]) || !finite(frame.luxelToWorld[1]))
        throw std::invalid_argument("invalid lightmap inverse");
    return frame;
}

std::array<std::vector<DVec3>, 2> clip(std::span<const DVec3> vertices, DVec3 normal, double dist) {
    std::array<std::vector<DVec3>, 2> result; // front, back, as in VBSP
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto a = vertices[i], b = vertices[(i + 1) % vertices.size()];
        const double da = glm::dot(a, normal) - dist, db = glm::dot(b, normal) - dist;
        const auto side = [](double d) { return d > 0.1 ? 1 : (d < -0.1 ? -1 : 0); };
        const int sa = side(da), sb = side(db); // VBSP ON_EPSILON, in world units
        if (sa >= 0) result[0].push_back(a);
        if (sa <= 0) result[1].push_back(a);
        if (sa * sb == -1) {
            const auto point = a + (b - a) * (da / (da - db));
            result[0].push_back(point);
            result[1].push_back(point);
        }
    }
    return result;
}

} // namespace

LuxelChart bspChart(std::array<DVec3, 2> worldToLuxel, DVec2 offset, std::array<int, 2> mins,
                    std::array<int, 2> extents, const Plane& plane) {
    if (!finite(worldToLuxel[0]) || !finite(worldToLuxel[1]) || !finite(plane.normal) ||
        !std::isfinite(plane.dist) || !std::isfinite(offset.x) || !std::isfinite(offset.y) ||
        extents[0] < 0 || extents[1] < 0 || extents[0] >= 1 << 20 || extents[1] >= 1 << 20)
        throw std::invalid_argument("invalid BSP lightmap frame");
    const double det = glm::dot(glm::cross(worldToLuxel[0], worldToLuxel[1]), plane.normal);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) throw std::invalid_argument("singular lightmap axes");
    LuxelChart frame;
    frame.worldToLuxel = worldToLuxel;
    frame.offset = offset;
    frame.mins = mins;
    frame.extents = extents;
    frame.dimensions = {extents[0] + 1, extents[1] + 1};
    solveInverse(frame, plane);
    if (!finite(frame.origin) || !finite(frame.luxelToWorld[0]) || !finite(frame.luxelToWorld[1]))
        throw std::invalid_argument("invalid lightmap inverse");
    return frame;
}

namespace {

int lightmapScale(const geometry::Face& face) {
    return face.lightmapScale == 0 ? 16 : std::max(1, face.lightmapScale);
}

// The face's loop and its lightmap frame (axes and offset), validated as chartFace documents.
std::vector<DVec3> faceFrame(const geometry::Face& face, std::span<const DVec3> vertices, LuxelChart& frame) {
    if (face.vertices.size() < 3 || !finite(face.plane.normal) || !std::isfinite(face.plane.dist) ||
        !finite(face.uv.uaxis) || !finite(face.uv.vaxis) || !std::isfinite(face.uv.shiftU) ||
        !std::isfinite(face.uv.shiftV) || !std::isfinite(face.uv.scaleU) || !std::isfinite(face.uv.scaleV))
        throw std::invalid_argument("invalid lightmap face");
    std::vector<DVec3> loop;
    for (const auto index : face.vertices) {
        if (index >= vertices.size() || !finite(vertices[index]))
            throw std::invalid_argument("invalid lightmap vertex");
        loop.push_back(vertices[index]);
    }
    const double scale = lightmapScale(face);
    frame.worldToLuxel = {face.uv.uaxis / scale, face.uv.vaxis / scale};
    // TexinfoForBrushTexture: texture scaling affects the shift, not the lightmap directions.
    frame.offset = {face.uv.shiftU * (face.uv.scaleU == 0 ? 1 : face.uv.scaleU) / scale,
                    face.uv.shiftV * (face.uv.scaleV == 0 ? 1 : face.uv.scaleV) / scale};
    const double det = glm::dot(glm::cross(face.uv.uaxis, face.uv.vaxis), face.plane.normal);
    if (!std::isfinite(det) || std::abs(det) < 1e-12)
        throw std::invalid_argument("singular lightmap axes");
    return loop;
}

} // namespace

FaceCharting chartFace(const geometry::Face& face, std::span<const DVec3> vertices) {
    LuxelChart frame;
    auto loop = faceFrame(face, vertices, frame);
    FaceCharting result;
    result.chart = chart(loop, frame, face.plane);
    std::vector<std::vector<DVec3>> pending;
    pending.push_back(std::move(loop));
    while (!pending.empty()) {
        auto points = std::move(pending.back());
        pending.pop_back();
        const auto b = bounds(points, frame);
        int axis = 0;
        while (axis < 2 && b.maxs[axis] - b.mins[axis] <= kBrushLightmapSpan) ++axis;
        if (axis < 2) {
            const auto direction = frame.worldToLuxel[axis];
            const double length = glm::length(direction);
            auto pieces = clip(points, direction / length, (b.mins[axis] + kBrushLightmapSpan - 1) / length);
            if (pieces[0].size() < 3 || pieces[1].size() < 3 ||
                bounds(pieces[0], frame).mins[axis] <= b.mins[axis])
                throw std::invalid_argument("lightmap subdivision did not progress");
            pending.push_back(std::move(pieces[1]));
            pending.push_back(std::move(pieces[0]));
            continue;
        }
        ChartedFace fragment;
        fragment.chart = chart(points, frame, face.plane);
        fragment.vertices = std::move(points);
        fragment.face = face;
        fragment.face.vertices.resize(fragment.vertices.size());
        std::iota(fragment.face.vertices.begin(), fragment.face.vertices.end(), std::size_t{0});
        result.fragments.push_back(std::move(fragment));
    }
    return result;
}

std::array<DVec3, 4> displacementGridCorners(std::span<const DVec3> corners, DVec3 start) {
    if (corners.size() != 4) throw std::invalid_argument("a displacement needs a four-sided base face");
    // As geometry::displacementMesh: Source winds clockwise from outside, from the corner nearest start.
    std::array<DVec3, 4> source;
    for (std::size_t k = 0; k < 4; ++k) source[k] = corners[3 - k];
    std::size_t first = 0;
    for (std::size_t k = 1; k < 4; ++k)
        if (glm::distance(source[k], start) < glm::distance(source[first], start)) first = k;
    std::array<DVec3, 4> result;
    for (std::size_t k = 0; k < 4; ++k) result[k] = source[(first + k) % 4];
    return result;
}

int displacementLuxelScale(const geometry::Face& face) {
    // CCoreDispSurface's luxels per world unit: float division and length, truncated (P0: 15 -> 14).
    const float scale = static_cast<float>(lightmapScale(face));
    const float x = static_cast<float>(face.uv.uaxis.x) / scale, y = static_cast<float>(face.uv.uaxis.y) / scale,
                z = static_cast<float>(face.uv.uaxis.z) / scale;
    const float length = std::sqrt(x * x + y * y + z * z);
    const float luxel = 1.0f / length;
    if (!std::isfinite(luxel) || luxel < 1.0f || luxel > float(1 << 20))
        throw std::invalid_argument("invalid displacement luxel scale");
    return static_cast<int>(luxel);
}

LuxelChart chartDisplacement(const geometry::Face& face, std::span<const DVec3> vertices,
                             const std::array<DVec3, 4>& c) {
    LuxelChart frame;
    const auto loop = faceFrame(face, vertices, frame);
    if (!std::ranges::all_of(c, finite)) throw std::invalid_argument("invalid displacement corner");
    auto result = chart(loop, frame, face.plane);
    const double scale = displacementLuxelScale(face);
    const double columns = std::max(glm::distance(c[0], c[3]), glm::distance(c[1], c[2])) / scale;
    const double rows = std::max(glm::distance(c[0], c[1]), glm::distance(c[3], c[2])) / scale;
    for (const double length : {columns, rows})
        if (!std::isfinite(length) || length >= (1 << 20)) throw std::invalid_argument("lightmap extents out of range");
    result.extents = {static_cast<int>(std::floor(columns)) + 1, static_cast<int>(std::floor(rows)) + 1};
    result.dimensions = {result.extents[0] + 1, result.extents[1] + 1};
    return result;
}

std::vector<DVec3> displacementNormals(const geometry::DisplacementMesh& mesh) {
    std::vector<DVec3> normals(mesh.positions.size(), DVec3(0.0));
    for (const auto& t : mesh.triangles) {
        const auto& p = mesh.positions;
        const DVec3 n = glm::cross(p[t[1]] - p[t[0]], p[t[2]] - p[t[0]]);
        for (const auto k : t) normals[k] += n;
    }
    for (auto& n : normals) {
        const double length = glm::length(n);
        n = length > 0.0 ? n / length : DVec3(0.0);
    }
    return normals;
}

SurfacePoint displacementSurfaceAt(const geometry::DisplacementMesh& mesh, std::span<const DVec3> normals,
                                   DVec2 fraction) {
    const int size = mesh.size;
    if (size < 2 || mesh.positions.size() != std::size_t(size) * std::size_t(size) ||
        normals.size() != mesh.positions.size())
        throw std::invalid_argument("invalid displacement mesh");
    const double gi = std::clamp(fraction.x, 0.0, 1.0) * (size - 1), gj = std::clamp(fraction.y, 0.0, 1.0) * (size - 1);
    const int r = std::min(int(gi), size - 2), c = std::min(int(gj), size - 2);
    const double fi = gi - r, fj = gj - c;
    // geometry::displacementMesh's split: an odd vertex index takes the (r, c+1)-(r+1, c) diagonal.
    std::array<std::array<int, 2>, 3> tri;
    if ((r * size + c) % 2) tri = fi + fj <= 1 ? std::array<std::array<int, 2>, 3>{{{0, 0}, {1, 0}, {0, 1}}}
                                               : std::array<std::array<int, 2>, 3>{{{1, 1}, {0, 1}, {1, 0}}};
    else tri = fi >= fj ? std::array<std::array<int, 2>, 3>{{{0, 0}, {1, 0}, {1, 1}}}
                        : std::array<std::array<int, 2>, 3>{{{0, 0}, {1, 1}, {0, 1}}};
    // Barycentric weights in grid space; the three corners are never collinear.
    const DVec2 a(tri[0][0], tri[0][1]), b(tri[1][0], tri[1][1]), d(tri[2][0], tri[2][1]), p(fi, fj);
    const double det = (b.x - a.x) * (d.y - a.y) - (d.x - a.x) * (b.y - a.y);
    const double wb = ((p.x - a.x) * (d.y - a.y) - (d.x - a.x) * (p.y - a.y)) / det;
    const double wd = ((b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y)) / det;
    const std::array<double, 3> weight{1.0 - wb - wd, wb, wd};
    SurfacePoint result{DVec3(0.0), DVec3(0.0)};
    for (std::size_t k = 0; k < 3; ++k) {
        const auto index = std::size_t((r + tri[k][0]) * size + c + tri[k][1]);
        result.position += mesh.positions[index] * weight[k];
        result.normal += normals[index] * weight[k];
    }
    const double length = glm::length(result.normal);
    if (length > 1e-9) result.normal /= length;
    else {
        const auto& q = mesh.positions;
        const auto at = [&](int k) { return q[std::size_t((r + tri[std::size_t(k)][0]) * size + c + tri[std::size_t(k)][1])]; };
        // Grid order (row, column) is clockwise from outside: the face normal is the reversed cross.
        const DVec3 n = glm::cross(at(2) - at(0), at(1) - at(0));
        result.normal = glm::length(n) > 0.0 ? glm::normalize(n) : DVec3(0.0, 0.0, 1.0);
    }
    return result;
}

std::vector<SurfacePoint> displacementLattice(const geometry::DisplacementMesh& mesh,
                                              const std::array<DVec3, 4>& c, std::array<int, 2> extents) {
    if (extents[0] < 1 || extents[1] < 1 || extents[0] >= 1 << 16 || extents[1] >= 1 << 16)
        throw std::invalid_argument("invalid displacement lightmap extents");
    const auto normals = displacementNormals(mesh);
    // 1 world unit inside the edges, in luxels along each grid direction (at most half a luxel).
    const std::array<double, 2> length{std::max(glm::distance(c[0], c[3]), glm::distance(c[1], c[2])),
                                       std::max(glm::distance(c[0], c[1]), glm::distance(c[3], c[2]))};
    std::array<double, 2> inset{};
    for (int axis = 0; axis < 2; ++axis)
        inset[axis] = length[axis] > 0.0 ? std::min(0.5, extents[axis] / length[axis]) : 0.5;
    std::vector<SurfacePoint> lattice;
    lattice.reserve(std::size_t(extents[0] + 2) * std::size_t(extents[1] + 2));
    for (int y = 0; y <= extents[1] + 1; ++y) for (int x = 0; x <= extents[0] + 1; ++x) {
        const double s = std::clamp(x - 0.5, inset[0], extents[0] - inset[0]);
        const double t = std::clamp(y - 0.5, inset[1], extents[1] - inset[1]);
        lattice.push_back(displacementSurfaceAt(mesh, normals, {t / extents[1], s / extents[0]}));
    }
    return lattice;
}

} // namespace apex::lighting
