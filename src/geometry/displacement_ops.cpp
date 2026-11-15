#include "geometry/displacement_ops.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace apex::geometry {
namespace {

Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }

bool finite(DVec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Hammer's allowed_verts covers the largest grid whatever the power: 17 x 17 bits set, padded
// to whole words (Strata Hammer writes five 64-bit words, test1_disp.vmf).
constexpr std::size_t kAllowedBitsSet = 289, kAllowedBitsStored = 320;

Result<std::vector<DVec3>> flatPositions(std::span<const DVec3> corners, const Displacement& disp) {
    auto flat = disp;
    std::ranges::fill(flat.distances, 0.0);
    std::ranges::fill(flat.offsets, DVec3{0.0});
    auto mesh = displacementMesh(corners, flat);
    if (!mesh) return std::unexpected(mesh.error());
    return std::move(mesh->positions);
}

// Moves vertex i by delta: the part along its normal goes into its distance, the rest into its
// offset, so normal * distance + offset grows by exactly delta.
void displaceBy(Displacement& disp, std::size_t i, DVec3 delta) {
    const auto& normal = disp.normals[i];
    const double length2 = glm::dot(normal, normal);
    if (length2 > 1e-18) {
        const double along = glm::dot(delta, normal) / length2;
        disp.distances[i] += along;
        delta -= normal * along;
    }
    disp.offsets[i] += delta;
}

// The four boundary edges of a size x size grid, each corner to corner.
std::array<std::vector<std::size_t>, 4> gridEdges(int size) {
    std::array<std::vector<std::size_t>, 4> edges;
    const auto n = static_cast<std::size_t>(size);
    for (std::size_t k = 0; k < n; ++k) {
        edges[0].push_back(k);                 // Row 0.
        edges[1].push_back((n - 1) * n + k);   // Last row.
        edges[2].push_back(k * n);             // Column 0.
        edges[3].push_back(k * n + n - 1);     // Last column.
    }
    return edges;
}

std::vector<std::size_t> boundary(int size) {
    std::vector<std::size_t> result;
    const auto n = static_cast<std::size_t>(size);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j)
            if (i == 0 || j == 0 || i + 1 == n || j + 1 == n) result.push_back(i * n + j);
    return result;
}

using Match = DisplacementVertexMatch;

struct Analysis {
    std::vector<std::vector<DVec3>> positions; // Current mesh, per patch.
    std::vector<Match> matches;
};

Result<Analysis> analyse(std::span<const DisplacementPatch> patches) {
    Analysis result;
    std::vector<std::vector<DVec3>> flats;
    for (const auto& patch : patches) {
        auto mesh = displacementMesh(patch.corners, patch.disp);
        if (!mesh) return std::unexpected(mesh.error());
        result.positions.push_back(std::move(mesh->positions));
        auto flat = flatPositions(patch.corners, patch.disp);
        if (!flat) return std::unexpected(flat.error());
        flats.push_back(std::move(*flat));
    }
    // Only patches whose base faces' boxes touch can share an edge.
    std::vector<std::pair<DVec3, DVec3>> boxes;
    for (const auto& patch : patches) {
        DVec3 low = patch.corners[0], high = patch.corners[0];
        for (const auto& c : patch.corners) { low = glm::min(low, c); high = glm::max(high, c); }
        boxes.emplace_back(low - DVec3{kSewBaseTolerance}, high + DVec3{kSewBaseTolerance});
    }
    const auto touching = [&](std::size_t p, std::size_t q) {
        return glm::all(glm::lessThanEqual(boxes[p].first, boxes[q].second)) &&
               glm::all(glm::lessThanEqual(boxes[q].first, boxes[p].second));
    };
    for (std::size_t p = 0; p < patches.size(); ++p) {
        for (const auto v : boundary(patches[p].disp.size())) {
            const auto x = flats[p][v];
            for (std::size_t q = 0; q < patches.size(); ++q) {
                if (q == p || !touching(p, q)) continue;
                const int m = patches[q].disp.size();
                for (const auto& edge : gridEdges(m)) {
                    const auto a = flats[q][edge.front()], b = flats[q][edge.back()];
                    const double length2 = glm::dot(b - a, b - a);
                    if (length2 <= kSewBaseTolerance * kSewBaseTolerance) continue;
                    const double length = std::sqrt(length2), t = glm::dot(x - a, b - a) / length2;
                    if (t * length < -kSewBaseTolerance || (t - 1.0) * length > kSewBaseTolerance) continue;
                    if (glm::distance(x, a + (b - a) * t) > kSewBaseTolerance) continue;
                    const double k = std::clamp(t, 0.0, 1.0) * (m - 1);
                    const double nearest = std::round(k);
                    if (std::abs(k - nearest) * length / (m - 1) <= kSewBaseTolerance) {
                        const auto w = edge[static_cast<std::size_t>(nearest)];
                        result.matches.push_back({p, v, q, w, w, 0.0});
                    } else {
                        const auto segment = std::min(static_cast<std::size_t>(std::floor(k)), edge.size() - 2);
                        result.matches.push_back({p, v, q, edge[segment], edge[segment + 1], k - double(segment)});
                    }
                    break; // A corner lies on two of q's edges; one match is enough.
                }
            }
        }
    }
    return result;
}

} // namespace

DVec3 displacementStartCorner(std::span<const DVec3> corners) {
    if (corners.empty()) return DVec3{0.0};
    DVec3 low = corners.front();
    for (const auto& c : corners) low = glm::min(low, c);
    std::size_t best = 0;
    for (std::size_t k = 1; k < corners.size(); ++k)
        if (glm::distance(corners[k], low) < glm::distance(corners[best], low)) best = k;
    return corners[best];
}

Result<DVec3> displacementBaseNormal(std::span<const DVec3> corners) {
    if (corners.size() != 4) return std::unexpected(invalid("a displacement needs a four-sided base face"));
    if (!std::ranges::all_of(corners, finite)) return std::unexpected(invalid("base face corner is not finite"));
    DVec3 normal{0.0};
    for (std::size_t k = 0; k < 4; ++k) {
        const auto& a = corners[k];
        const auto& b = corners[(k + 1) % 4];
        normal += DVec3{(a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)};
    }
    const double length = glm::length(normal);
    if (!(length > 1e-12)) return std::unexpected(invalid("the base face has no area"));
    return normal / length;
}

Result<Displacement> createDisplacement(std::span<const DVec3> corners, int power, double elevation) {
    if (power < Displacement::kMinPower || power > Displacement::kMaxPower)
        return std::unexpected(invalid("displacement power must be 2, 3 or 4"));
    if (!std::isfinite(elevation)) return std::unexpected(invalid("displacement elevation is not finite"));
    const auto normal = displacementBaseNormal(corners);
    if (!normal) return std::unexpected(normal.error());
    Displacement d;
    d.power = power;
    d.startPosition = displacementStartCorner(corners);
    d.elevation = elevation;
    const auto n = d.vertexCount();
    d.normals.assign(n, *normal);
    d.distances.assign(n, 0.0);
    d.offsets.assign(n, DVec3{0.0});
    d.offsetNormals.assign(n, *normal);
    d.alphas.assign(n, 0.0);
    d.triangleTags.assign(d.triangleCount(), 0);
    d.allowedVerts.assign(kAllowedBitsStored, false);
    std::fill_n(d.allowedVerts.begin(), kAllowedBitsSet, true);
    if (auto tagged = retagDisplacement(corners, d); !tagged) return std::unexpected(tagged.error());
    return d;
}

Result<void> retagDisplacement(std::span<const DVec3> corners, Displacement& disp) {
    const auto mesh = displacementMesh(corners, disp);
    if (!mesh) return std::unexpected(mesh.error());
    const auto apply = [](int tag, int bit, int force, int forced, bool slope) {
        const bool on = (tag & force) ? (tag & forced) != 0 : slope;
        return on ? tag | bit : tag & ~bit;
    };
    for (std::size_t t = 0; t < mesh->triangles.size(); ++t) {
        const auto& tri = mesh->triangles[t];
        const auto& a = mesh->positions[tri[0]];
        const auto cross = glm::cross(mesh->positions[tri[1]] - a, mesh->positions[tri[2]] - a);
        const double length = glm::length(cross);
        const double z = length > 0.0 ? cross.z / length : 0.0;
        int tag = disp.triangleTags[t];
        tag = apply(tag, Displacement::kTagWalkable, Displacement::kTagForceWalkable,
                    Displacement::kTagForceWalkableValue, z >= kWalkableNormalZ);
        tag = apply(tag, Displacement::kTagBuildable, Displacement::kTagForceBuildable,
                    Displacement::kTagForceBuildableValue, z >= kBuildableNormalZ);
        disp.triangleTags[t] = tag;
    }
    return {};
}

// Moves smaller than this are rounding left by an earlier sew, not a gap.
constexpr double kSewNoise = 1e-9;

Result<std::size_t> sewDisplacements(std::span<DisplacementPatch> patches) {
    const auto analysis = analyse(patches);
    if (!analysis) return std::unexpected(analysis.error());
    const auto& positions = analysis->positions;
    std::vector<std::size_t> base(patches.size() + 1, 0);
    for (std::size_t p = 0; p < patches.size(); ++p) base[p + 1] = base[p] + patches[p].disp.vertexCount();
    std::vector<std::size_t> parent(base.back());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    const auto find = [&](std::size_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    for (const auto& m : analysis->matches)
        if (m.coincident()) parent[find(base[m.p] + m.v)] = find(base[m.q] + m.w);

    std::map<std::size_t, std::vector<std::pair<std::size_t, std::size_t>>> groups;
    for (const auto& m : analysis->matches) {
        groups[find(base[m.p] + m.v)].emplace_back(m.p, m.v);
        if (m.coincident()) groups[find(base[m.q] + m.w)].emplace_back(m.q, m.w);
    }
    std::map<std::pair<std::size_t, std::size_t>, DVec3> target;
    std::map<std::size_t, DVec3> groupTarget;
    for (auto& [root, members] : groups) {
        std::ranges::sort(members);
        members.erase(std::unique(members.begin(), members.end()), members.end());
        DVec3 sum{0.0};
        for (const auto& [p, v] : members) sum += positions[p][v];
        groupTarget[root] = sum / double(members.size());
    }
    const auto averaged = [&](std::size_t p, std::size_t v) {
        const auto found = groupTarget.find(find(base[p] + v));
        return found == groupTarget.end() ? positions[p][v] : found->second;
    };
    // T-edges: the finer side's extra vertex goes onto the line between the coarser side's two
    // (already averaged) vertices, and takes its group with it.
    std::map<std::size_t, std::pair<DVec3, int>> onLine;
    for (const auto& m : analysis->matches) {
        if (m.coincident()) continue;
        const auto point = averaged(m.q, m.w) + (averaged(m.q, m.w2) - averaged(m.q, m.w)) * m.s;
        auto& [sum, count] = onLine[find(base[m.p] + m.v)];
        sum += point;
        ++count;
    }
    for (const auto& [root, line] : onLine) groupTarget[root] = line.first / double(line.second);

    std::size_t changed = 0;
    for (std::size_t p = 0; p < patches.size(); ++p) {
        auto& disp = patches[p].disp;
        bool moved = false;
        for (const auto v : boundary(disp.size())) {
            const auto found = groupTarget.find(find(base[p] + v));
            if (found == groupTarget.end()) continue;
            const auto delta = found->second - positions[p][v];
            // Already sewn, up to rounding: a second sew is a no-op.
            if (glm::dot(delta, delta) < kSewNoise * kSewNoise) continue;
            displaceBy(disp, v, delta);
            moved = true;
        }
        if (!moved) continue;
        if (auto tagged = retagDisplacement(patches[p].corners, disp); !tagged) return std::unexpected(tagged.error());
        ++changed;
    }
    return changed;
}

Result<std::vector<DisplacementVertexMatch>> displacementVertexMatches(std::span<const DisplacementPatch> patches) {
    auto analysis = analyse(patches);
    if (!analysis) return std::unexpected(analysis.error());
    return std::move(analysis->matches);
}

Result<std::vector<DisplacementSeam>> displacementSeams(std::span<const DisplacementPatch> patches) {
    const auto analysis = analyse(patches);
    if (!analysis) return std::unexpected(analysis.error());
    const auto& positions = analysis->positions;
    struct Pair { std::size_t forward = 0, backward = 0; double gap = 0.0; };
    std::map<std::pair<std::size_t, std::size_t>, Pair> pairs;
    for (const auto& m : analysis->matches) {
        const auto& w = positions[m.q][m.w];
        const auto other = w + (positions[m.q][m.w2] - w) * m.s;
        auto& pair = pairs[{std::min(m.p, m.q), std::max(m.p, m.q)}];
        (m.p < m.q ? pair.forward : pair.backward) += 1;
        pair.gap = std::max(pair.gap, glm::distance(positions[m.p][m.v], other));
    }
    std::vector<DisplacementSeam> result;
    for (const auto& [key, pair] : pairs)
        if (pair.forward >= 2 || pair.backward >= 2) result.push_back({key.first, key.second, pair.gap});
    return result;
}

bool displacementIsFlat(const Displacement& disp) {
    return std::ranges::all_of(disp.distances, [](double d) { return d == 0.0; }) &&
           std::ranges::all_of(disp.offsets, [](DVec3 o) { return o == DVec3{0.0}; }) &&
           std::ranges::all_of(disp.alphas, [](double a) { return a == 0.0; });
}

Result<Displacement> resampleDisplacement(std::span<const DVec3> corners, const Displacement& disp, int power) {
    if (power < Displacement::kMinPower || power > Displacement::kMaxPower)
        return std::unexpected(invalid("displacement power must be 2, 3 or 4"));
    if (const auto problem = disp.problem()) return std::unexpected(invalid(*problem));
    const auto baseNormal = displacementBaseNormal(corners);
    if (!baseNormal) return std::unexpected(baseNormal.error());
    Displacement result = disp;
    result.power = power;
    const auto n = static_cast<std::size_t>(disp.size());
    const auto m = static_cast<std::size_t>(result.size());
    result.normals.assign(m * m, *baseNormal);
    result.distances.assign(m * m, 0.0);
    result.offsets.assign(m * m, DVec3{0.0});
    result.offsetNormals.assign(m * m, *baseNormal);
    result.alphas.assign(m * m, 0.0);
    // Both sizes are 2^p + 1, so the scale is a power of two and grid coordinates are exact.
    const double scale = double(n - 1) / double(m - 1);
    const auto cell = [&](double x) { return std::min(static_cast<std::size_t>(x), n - 2); };
    for (std::size_t i = 0; i < m; ++i)
        for (std::size_t j = 0; j < m; ++j) {
            const auto at = i * m + j;
            const double x = double(i) * scale, y = double(j) * scale;
            if (x == std::floor(x) && y == std::floor(y)) {
                const auto from = static_cast<std::size_t>(x) * n + static_cast<std::size_t>(y);
                result.normals[at] = disp.normals[from];
                result.distances[at] = disp.distances[from];
                result.offsets[at] = disp.offsets[from];
                result.offsetNormals[at] = disp.offsetNormals[from];
                result.alphas[at] = disp.alphas[from];
                continue;
            }
            const auto i0 = cell(x), j0 = cell(y);
            const double fx = x - double(i0), fy = y - double(j0);
            const std::array<std::size_t, 4> from{i0 * n + j0, i0 * n + j0 + 1, (i0 + 1) * n + j0, (i0 + 1) * n + j0 + 1};
            const std::array<double, 4> weight{(1 - fx) * (1 - fy), (1 - fx) * fy, fx * (1 - fy), fx * fy};
            DVec3 vector{0.0}, offsetNormal{0.0};
            for (std::size_t k = 0; k < 4; ++k) {
                vector += disp.normals[from[k]] * disp.distances[from[k]] * weight[k];
                offsetNormal += disp.offsetNormals[from[k]] * weight[k];
                result.offsets[at] += disp.offsets[from[k]] * weight[k];
                result.alphas[at] += disp.alphas[from[k]] * weight[k];
            }
            if (const double length = glm::length(vector); length > kSewNoise) {
                result.normals[at] = vector / length;
                result.distances[at] = length;
            }
            if (const double length = glm::length(offsetNormal); length > kSewNoise)
                result.offsetNormals[at] = offsetNormal / length;
        }
    constexpr int kForceBits = Displacement::kTagForceWalkable | Displacement::kTagForceWalkableValue |
                               Displacement::kTagForceBuildable | Displacement::kTagForceBuildableValue;
    result.triangleTags.assign(result.triangleCount(), 0);
    for (std::size_t r = 0; r + 1 < m; ++r)
        for (std::size_t c = 0; c + 1 < m; ++c) {
            const auto old = cell(double(r) * scale) * (n - 1) + cell(double(c) * scale);
            for (std::size_t k = 0; k < 2; ++k)
                result.triangleTags[2 * (r * (m - 1) + c) + k] = disp.triangleTags[2 * old + k] & kForceBits;
        }
    if (auto tagged = retagDisplacement(corners, result); !tagged) return std::unexpected(tagged.error());
    return result;
}

} // namespace apex::geometry
