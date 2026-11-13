#include "geometry/displacement.hpp"

#include "geometry/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace apex::geometry {
namespace {

bool finite(DVec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }

// VBSP's two triangles per quad, alternating the diagonal by vertex-index parity, in
// Source's clockwise winding.
std::vector<std::array<std::uint32_t, 3>> sourceTriangles(int size) {
    std::vector<std::array<std::uint32_t, 3>> result;
    const auto w = static_cast<std::uint32_t>(size);
    for (std::uint32_t r = 0; r + 1 < w; ++r) {
        for (std::uint32_t c = 0; c + 1 < w; ++c) {
            const auto i = r * w + c;
            if (i % 2) {
                result.push_back({i, i + w, i + 1});
                result.push_back({i + 1, i + w, i + w + 1});
            } else {
                result.push_back({i, i + w, i + w + 1});
                result.push_back({i, i + w + 1, i + 1});
            }
        }
    }
    return result;
}

std::array<std::uint32_t, 3> sorted(std::array<std::uint32_t, 3> t) {
    std::ranges::sort(t);
    return t;
}

} // namespace

std::optional<std::string> Displacement::problem() const {
    if (power < kMinPower || power > kMaxPower)
        return "displacement power " + std::to_string(power) + " is outside 2..4";
    const auto n = vertexCount();
    if (normals.size() != n || distances.size() != n || offsets.size() != n || offsetNormals.size() != n ||
        alphas.size() != n)
        return "displacement rows do not match power " + std::to_string(power);
    if (triangleTags.size() != triangleCount()) return "displacement triangle_tags do not match its power";
    if (!finite(startPosition) || !std::isfinite(elevation)) return "displacement start or elevation is not finite";
    for (std::size_t i = 0; i < n; ++i)
        if (!finite(normals[i]) || !std::isfinite(distances[i]) || !finite(offsets[i]) ||
            !finite(offsetNormals[i]) || !std::isfinite(alphas[i]))
            return "displacement vertex " + std::to_string(i) + " is not finite";
    return std::nullopt;
}

Result<DisplacementMesh> displacementMesh(std::span<const DVec3> corners, const Displacement& disp) {
    if (const auto problem = disp.problem()) return std::unexpected(invalid(*problem));
    // VBSP rejects other faces: "Trying to create a non-quad displacement!".
    if (corners.size() != 4) return std::unexpected(invalid("a displacement needs a four-sided base face"));
    if (!std::ranges::all_of(corners, finite)) return std::unexpected(invalid("base face corner is not finite"));
    // Source winding is clockwise from outside: the reverse of corners.
    std::array<DVec3, 4> source;
    for (std::size_t k = 0; k < 4; ++k) source[k] = corners[3 - k];
    std::size_t start = 0;
    for (std::size_t k = 1; k < 4; ++k)
        if (glm::distance(source[k], disp.startPosition) < glm::distance(source[start], disp.startPosition)) start = k;
    std::array<DVec3, 4> c;
    for (std::size_t k = 0; k < 4; ++k) c[k] = source[(start + k) % 4];

    DisplacementMesh mesh;
    mesh.size = disp.size();
    const int size = disp.size();
    mesh.positions.reserve(disp.vertexCount());
    for (int i = 0; i < size; ++i) {
        const double t = double(i) / (size - 1);
        const DVec3 left = c[0] + (c[1] - c[0]) * t, right = c[3] + (c[2] - c[3]) * t;
        for (int j = 0; j < size; ++j) {
            const auto index = static_cast<std::size_t>(i * size + j);
            const DVec3 flat = left + (right - left) * (double(j) / (size - 1));
            mesh.positions.push_back(flat + disp.normals[index] * disp.distances[index] + disp.offsets[index]);
        }
    }
    mesh.alphas = disp.alphas;
    for (auto t : sourceTriangles(size)) mesh.triangles.push_back({t[0], t[2], t[1]});
    mesh.tags = disp.triangleTags;
    return mesh;
}

Result<std::array<DVec3, 4>> displacementCorners(const ConvexPolyhedron& poly, const Face& face) {
    std::vector<DVec3> loop;
    for (const auto index : face.vertices) {
        if (index >= poly.vertices.size()) return std::unexpected(invalid("face vertex index out of range"));
        loop.push_back(poly.vertices[index]);
    }
    std::vector<DVec3> corners;
    for (std::size_t k = 0; k < loop.size(); ++k) {
        const auto& prev = loop[(k + loop.size() - 1) % loop.size()];
        const auto& next = loop[(k + 1) % loop.size()];
        const auto a = loop[k] - prev, b = next - loop[k];
        // Collinear subdivision vertices have no turn; the tolerance is an area per unit length.
        if (glm::length(glm::cross(a, b)) > kMinEdgeLength * std::max(glm::length(a), glm::length(b)))
            corners.push_back(loop[k]);
    }
    if (corners.size() != 4) return std::unexpected(invalid("a displacement needs a four-sided base face"));
    return std::array<DVec3, 4>{corners[0], corners[1], corners[2], corners[3]};
}

Displacement transformDisplacement(const Displacement& disp, const DMat3& linear, DVec3 translation) {
    if (const auto problem = disp.problem()) throw std::invalid_argument(*problem);
    const double determinant = glm::determinant(linear);
    if (!std::isfinite(determinant) || determinant == 0.0 || !finite(translation))
        throw std::invalid_argument("displacement transform must be finite and invertible");
    Displacement result = disp;
    result.startPosition = linear * disp.startPosition + translation;
    for (std::size_t i = 0; i < disp.vertexCount(); ++i) {
        // n * d is a displacement vector: A(n d) = (A n / |A n|) (d |A n|). Zero normals stay zero.
        const auto mapped = linear * disp.normals[i];
        const double length = glm::length(mapped);
        if (length > 0.0) {
            result.normals[i] = mapped / length;
            result.distances[i] = disp.distances[i] * length;
        }
        result.offsets[i] = linear * disp.offsets[i];
        const auto offsetNormal = linear * disp.offsetNormals[i];
        const double offsetLength = glm::length(offsetNormal);
        if (offsetLength > 0.0) result.offsetNormals[i] = offsetNormal / offsetLength;
    }
    if (determinant > 0.0) return result;

    // Mirror: Source's row edge (c0->c1) and column edge (c0->c3) trade places, so vertex
    // (r, c) moves to (c, r). The quad parity rule is symmetric, so triangles map to triangles.
    const int size = disp.size();
    const auto moved = [size](std::size_t index) {
        const auto r = index / static_cast<std::size_t>(size), c = index % static_cast<std::size_t>(size);
        return c * static_cast<std::size_t>(size) + r;
    };
    const auto transposed = result;
    for (std::size_t i = 0; i < disp.vertexCount(); ++i) {
        const auto j = moved(i);
        result.normals[j] = transposed.normals[i];
        result.distances[j] = transposed.distances[i];
        result.offsets[j] = transposed.offsets[i];
        result.offsetNormals[j] = transposed.offsetNormals[i];
        result.alphas[j] = transposed.alphas[i];
        if (i < disp.allowedVerts.size() && j < disp.allowedVerts.size())
            result.allowedVerts[j] = disp.allowedVerts[i];
    }
    const auto triangles = sourceTriangles(size);
    std::map<std::array<std::uint32_t, 3>, std::size_t> byVertices;
    for (std::size_t t = 0; t < triangles.size(); ++t) byVertices.emplace(sorted(triangles[t]), t);
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        auto image = triangles[t];
        for (auto& v : image) v = static_cast<std::uint32_t>(moved(v));
        const auto found = byVertices.find(sorted(image));
        if (found == byVertices.end()) throw std::logic_error("displacement triangulation is not transpose-symmetric");
        result.triangleTags[found->second] = disp.triangleTags[t];
    }
    return result;
}

} // namespace apex::geometry
