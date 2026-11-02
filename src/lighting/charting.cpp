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

FaceCharting chartFace(const geometry::Face& face, std::span<const DVec3> vertices) {
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
    const double scale = face.lightmapScale == 0 ? 16 : std::max(1, face.lightmapScale);
    LuxelChart frame;
    frame.worldToLuxel = {face.uv.uaxis / scale, face.uv.vaxis / scale};
    // TexinfoForBrushTexture: texture scaling affects the shift, not the lightmap directions.
    frame.offset = {face.uv.shiftU * (face.uv.scaleU == 0 ? 1 : face.uv.scaleU) / scale,
                    face.uv.shiftV * (face.uv.scaleV == 0 ? 1 : face.uv.scaleV) / scale};
    const double det = glm::dot(glm::cross(face.uv.uaxis, face.uv.vaxis), face.plane.normal);
    if (!std::isfinite(det) || std::abs(det) < 1e-12)
        throw std::invalid_argument("singular lightmap axes");
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

} // namespace apex::lighting
