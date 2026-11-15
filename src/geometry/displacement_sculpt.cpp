#include "geometry/displacement_sculpt.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace apex::geometry {
namespace {

Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }

// Below this a displacement vector is zero: Hammer writes normal 0 0 0 and distance 0.
constexpr double kZeroVector = 1e-9;

std::uint64_t mix(std::uint64_t x) { // splitmix64's finaliser.
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

std::uint64_t quantised(double value) { return static_cast<std::uint64_t>(std::llround(value * 16.0)); }

DVec3 axis(SculptDirection direction) {
    switch (direction) {
    case SculptDirection::AxisX: return {1, 0, 0};
    case SculptDirection::AxisY: return {0, 1, 0};
    default: return {0, 0, 1};
    }
}

bool isAxis(SculptDirection direction) {
    return direction == SculptDirection::AxisX || direction == SculptDirection::AxisY ||
           direction == SculptDirection::AxisZ;
}

DVec3 unitOr(DVec3 v, DVec3 fallback) {
    const double length = glm::length(v);
    return length > 1e-12 ? v / length : fallback;
}

bool sphereTouches(const AABB& box, DVec3 centre, double radius) {
    if (box.empty()) return false;
    const DVec3 nearest = glm::clamp(centre, box.min, box.max);
    return glm::dot(nearest - centre, nearest - centre) <= radius * radius;
}

// Slab test; true when the ray (t >= 0) meets the box.
bool rayTouches(const AABB& box, const Ray& ray) {
    if (box.empty()) return false;
    double low = 0.0, high = std::numeric_limits<double>::infinity();
    for (int k = 0; k < 3; ++k) {
        const double lo = box.min[k] - 1e-6, hi = box.max[k] + 1e-6;
        if (std::abs(ray.dir[k]) < 1e-300) {
            if (ray.origin[k] < lo || ray.origin[k] > hi) return false;
            continue;
        }
        double a = (lo - ray.origin[k]) / ray.dir[k], b = (hi - ray.origin[k]) / ray.dir[k];
        if (a > b) std::swap(a, b);
        low = std::max(low, a);
        high = std::min(high, b);
        if (low > high) return false;
    }
    return true;
}

} // namespace

double brushWeight(BrushFalloff falloff, double distance, double radius) {
    if (!(radius > 0.0) || !(distance < radius)) return 0.0;
    const double x = std::max(distance, 0.0) / radius;
    switch (falloff) {
    case BrushFalloff::Constant: return 1.0;
    case BrushFalloff::Linear: return 1.0 - x;
    case BrushFalloff::Smooth: {
        const double y = 1.0 - x * x;
        return y * y;
    }
    }
    return 0.0;
}

void setDisplacementVector(Displacement& disp, std::size_t i, DVec3 vector) {
    const double length = glm::length(vector);
    if (!(length > kZeroVector)) {
        disp.normals[i] = DVec3{0.0};
        disp.distances[i] = 0.0;
        return;
    }
    disp.normals[i] = vector / length;
    disp.distances[i] = length;
}

Result<DisplacementStroke> DisplacementStroke::create(std::vector<DisplacementPatch> patches) {
    DisplacementStroke stroke;
    stroke.patches_.reserve(patches.size());
    std::size_t total = 0;
    for (auto& value : patches) {
        Patch patch;
        auto mesh = displacementMesh(value.corners, value.disp);
        if (!mesh) return std::unexpected(mesh.error());
        auto flat = value.disp;
        std::ranges::fill(flat.distances, 0.0);
        std::ranges::fill(flat.offsets, DVec3{0.0});
        auto flatMesh = displacementMesh(value.corners, flat);
        if (!flatMesh) return std::unexpected(flatMesh.error());
        const auto normal = displacementBaseNormal(value.corners);
        if (!normal) return std::unexpected(normal.error());
        patch.flat = std::move(flatMesh->positions);
        patch.positions = std::move(mesh->positions);
        patch.baseNormal = *normal;
        stroke.triangles_.try_emplace(value.disp.power, std::move(mesh->triangles));
        patch.value = std::move(value);
        stroke.offsets_.push_back(total);
        total += patch.positions.size();
        stroke.patches_.push_back(std::move(patch));
    }
    for (std::size_t p = 0; p < stroke.patches_.size(); ++p) stroke.refreshBounds(p);

    // Vertex groups: Sew's coincident matches, joined.
    std::vector<DisplacementPatch> values;
    values.reserve(stroke.patches_.size());
    for (const auto& patch : stroke.patches_) values.push_back(patch.value);
    const auto matches = displacementVertexMatches(values);
    if (!matches) return std::unexpected(matches.error());
    std::vector<std::size_t> parent(total);
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    const auto find = [&](std::size_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    for (const auto& m : *matches)
        if (m.coincident()) parent[find(stroke.global(m.p, m.v))] = find(stroke.global(m.q, m.w));
    std::vector<std::uint32_t> dense(total, ~0u);
    stroke.groupOf_.resize(total);
    std::uint32_t groups = 0;
    for (std::size_t g = 0; g < total; ++g) {
        auto& id = dense[find(g)];
        if (id == ~0u) id = groups++;
        stroke.groupOf_[g] = id;
    }
    stroke.groupStart_.assign(groups + 1, 0);
    for (const auto id : stroke.groupOf_) ++stroke.groupStart_[id + 1];
    std::partial_sum(stroke.groupStart_.begin(), stroke.groupStart_.end(), stroke.groupStart_.begin());
    stroke.members_.resize(total);
    auto next = stroke.groupStart_;
    for (std::size_t p = 0; p < stroke.patches_.size(); ++p)
        for (std::size_t v = 0; v < stroke.patches_[p].positions.size(); ++v)
            stroke.members_[next[stroke.groupOf_[stroke.global(p, v)]]++] = {static_cast<std::uint32_t>(p),
                                                                            static_cast<std::uint32_t>(v)};
    for (const auto& m : *matches)
        if (!m.coincident())
            stroke.tEdges_.push_back({stroke.groupOf_[stroke.global(m.p, m.v)], m.q, m.w, m.w2, m.s});
    stroke.stamp_.assign(groups, 0);
    return stroke;
}

void DisplacementStroke::setEditable(const std::vector<bool>& editable) {
    for (std::size_t p = 0; p < patches_.size(); ++p) patches_[p].editable = p >= editable.size() || editable[p];
}

void DisplacementStroke::refreshBounds(std::size_t p) {
    auto& patch = patches_[p];
    patch.bounds = {};
    for (const auto& position : patch.positions) patch.bounds.expand(position);
}

void DisplacementStroke::refreshNormals(std::size_t p) {
    auto& patch = patches_[p];
    if (!patch.normalsDirty) return;
    // Area-weighted, as the render mirror draws them.
    patch.normals.assign(patch.positions.size(), DVec3{0.0});
    for (const auto& t : triangles_.at(patch.value.disp.power)) {
        const auto& a = patch.positions[t[0]];
        const DVec3 n = glm::cross(patch.positions[t[1]] - a, patch.positions[t[2]] - a);
        for (const auto k : t) patch.normals[k] += n;
    }
    for (auto& normal : patch.normals) normal = unitOr(normal, patch.baseNormal);
    patch.normalsDirty = false;
}

std::optional<DisplacementStroke::Hit> DisplacementStroke::raycast(const Ray& ray) const {
    std::optional<Hit> best;
    for (std::size_t p = 0; p < patches_.size(); ++p) {
        const auto& patch = patches_[p];
        if (!rayTouches(patch.bounds, ray)) continue;
        for (const auto& t : triangles_.at(patch.value.disp.power)) {
            // Möller-Trumbore, both sides.
            const auto& a = patch.positions[t[0]];
            const DVec3 e1 = patch.positions[t[1]] - a, e2 = patch.positions[t[2]] - a;
            const DVec3 h = glm::cross(ray.dir, e2);
            const double det = glm::dot(e1, h);
            if (std::abs(det) < 1e-12) continue;
            const double inv = 1.0 / det;
            const DVec3 s = ray.origin - a;
            const double u = glm::dot(s, h) * inv;
            if (u < 0.0 || u > 1.0) continue;
            const DVec3 q = glm::cross(s, e1);
            const double v = glm::dot(ray.dir, q) * inv;
            if (v < 0.0 || u + v > 1.0) continue;
            const double distance = glm::dot(e2, q) * inv;
            if (distance <= 0.0 || (best && distance >= best->t)) continue;
            best = Hit{p, ray.at(distance), unitOr(glm::cross(e1, e2), patch.baseNormal), distance};
        }
    }
    return best;
}

void DisplacementStroke::begin() {
    originals_.clear();
    moved_.clear();
    plane_.reset();
    start_.reset();
    dabs_ = 0;
}

void DisplacementStroke::touch(std::size_t p) {
    if (!originals_.contains(p)) originals_.emplace(p, patches_[p].value.disp);
}

std::vector<DisplacementStroke::Affected> DisplacementStroke::affected(DVec3 centre, double radius,
                                                                      BrushFalloff falloff) {
    std::vector<Affected> result;
    ++clock_;
    for (std::size_t p = 0; p < patches_.size(); ++p) {
        const auto& patch = patches_[p];
        if (!sphereTouches(patch.bounds, centre, radius)) continue;
        for (std::size_t v = 0; v < patch.positions.size(); ++v) {
            const double weight = brushWeight(falloff, glm::distance(patch.positions[v], centre), radius);
            if (!(weight > 0.0)) continue;
            const auto group = groupOf_[global(p, v)];
            if (stamp_[group] == clock_) continue;
            stamp_[group] = clock_;
            const auto shared = members(group);
            if (std::ranges::none_of(shared, [&](const auto& m) { return patches_[m.first].editable; })) continue;
            result.push_back({group, weight});
        }
    }
    return result;
}

DVec3 DisplacementStroke::direction(SculptDirection mode, std::uint32_t group) {
    if (isAxis(mode)) return axis(mode);
    DVec3 face{0.0}, vertex{0.0};
    for (const auto& [p, v] : members(group)) {
        face += patches_[p].baseNormal;
        if (mode == SculptDirection::VertexNormal) {
            refreshNormals(p);
            vertex += patches_[p].normals[v];
        }
    }
    const DVec3 faceNormal = unitOr(face, DVec3{0, 0, 1});
    return mode == SculptDirection::VertexNormal ? unitOr(vertex, faceNormal) : faceNormal;
}

void DisplacementStroke::move(std::uint32_t group, DVec3 delta) {
    for (const auto& [p, v] : members(group)) {
        touch(p);
        auto& patch = patches_[p];
        auto& disp = patch.value.disp;
        setDisplacementVector(disp, v, disp.normals[v] * disp.distances[v] + delta);
        // displacementMesh's own expression, so the live mesh is the committed one bit for bit.
        patch.positions[v] = patch.flat[v] + disp.normals[v] * disp.distances[v] + disp.offsets[v];
        patch.normalsDirty = true;
        moved_.insert(p);
    }
}

void DisplacementStroke::sculpt(const SculptSettings& settings, DVec3 centre) {
    if (!start_) start_ = centre;
    const auto hits = affected(centre, settings.radius, settings.falloff);
    ++dabs_;
    if (hits.empty()) return;
    const auto rep = [&](std::uint32_t group) { return members(group).front(); };
    const auto position = [&](std::uint32_t group) {
        const auto [p, v] = rep(group);
        return patches_[p].positions[v];
    };
    if (settings.brush == SculptBrush::Flatten && !plane_) {
        DVec3 normal = axis(settings.direction);
        if (!isAxis(settings.direction)) {
            DVec3 sum{0.0};
            for (const auto& hit : hits) sum += direction(settings.direction, hit.group) * hit.weight;
            normal = unitOr(sum, DVec3{0, 0, 1});
        }
        plane_ = {centre, normal};
    }
    const std::uint64_t seed = mix(mix(mix(mix(settings.seed) ^ quantised(start_->x)) ^ quantised(start_->y)) ^
                                   quantised(start_->z)) ^ dabs_;

    // Every target is read before anything moves, so the order of groups does not matter.
    std::vector<DVec3> deltas;
    deltas.reserve(hits.size());
    for (const auto& [group, weight] : hits) {
        DVec3 delta{0.0};
        switch (settings.brush) {
        case SculptBrush::RaiseLower:
            delta = direction(settings.direction, group) * (settings.amount * weight);
            break;
        case SculptBrush::Noise: {
            const double unit = double(mix(seed ^ group) >> 11) * 0x1.0p-53; // [0, 1)
            delta = direction(settings.direction, group) * (settings.amount * weight * (2.0 * unit - 1.0));
            break;
        }
        case SculptBrush::Smooth: {
            DVec3 sum{0.0};
            int count = 0;
            for (const auto& [p, v] : members(group)) {
                const auto n = static_cast<std::size_t>(patches_[p].value.disp.size());
                const std::size_t r = v / n, c = v % n;
                const auto& at = patches_[p].positions;
                if (r > 0) { sum += at[v - n]; ++count; }
                if (r + 1 < n) { sum += at[v + n]; ++count; }
                if (c > 0) { sum += at[v - 1]; ++count; }
                if (c + 1 < n) { sum += at[v + 1]; ++count; }
            }
            // Along the direction only, so vertices do not slide across the surface.
            const DVec3 dir = direction(settings.direction, group);
            delta = dir * (glm::dot(sum / double(std::max(count, 1)) - position(group), dir) * settings.strength * weight);
            break;
        }
        case SculptBrush::Flatten: {
            const auto& [point, normal] = *plane_;
            delta = -normal * (glm::dot(position(group) - point, normal) * settings.strength * weight);
            break;
        }
        case SculptBrush::SetHeight: {
            const auto mode = settings.direction == SculptDirection::VertexNormal ? SculptDirection::FaceNormal
                                                                                  : settings.direction;
            const DVec3 dir = direction(mode, group);
            const auto [p, v] = rep(group);
            const double height = glm::dot(patches_[p].positions[v] - patches_[p].flat[v], dir);
            delta = dir * ((settings.height - height) * settings.strength * weight);
            break;
        }
        }
        deltas.push_back(delta);
    }
    std::set<std::size_t> changed;
    for (std::size_t i = 0; i < hits.size(); ++i) {
        if (glm::dot(deltas[i], deltas[i]) == 0.0) continue;
        move(hits[i].group, deltas[i]);
        for (const auto& member : members(hits[i].group)) changed.insert(member.first);
    }
    // A T-edge vertex stays on the coarser side's edge, as Sew leaves it.
    for (const auto& edge : tEdges_) {
        const auto [p, v] = rep(edge.group);
        if (!changed.contains(p) && !changed.contains(edge.q)) continue;
        const auto& coarse = patches_[edge.q].positions;
        const DVec3 target = coarse[edge.w] + (coarse[edge.w2] - coarse[edge.w]) * edge.s;
        const DVec3 delta = target - patches_[p].positions[v];
        if (glm::dot(delta, delta) <= 1e-24) continue;
        move(edge.group, delta);
        for (const auto& member : members(edge.group)) changed.insert(member.first);
    }
    for (const auto p : changed) refreshBounds(p);
}

void DisplacementStroke::paint(const PaintSettings& settings, DVec3 centre) {
    if (!start_) start_ = centre;
    const auto hits = affected(centre, settings.radius, settings.falloff);
    ++dabs_;
    const double target = std::clamp(settings.alpha, 0.0, 255.0);
    for (const auto& [group, weight] : hits) {
        const auto [p0, v0] = members(group).front();
        const double from = std::clamp(patches_[p0].value.disp.alphas[v0], 0.0, 255.0);
        const double alpha = from + (target - from) * std::clamp(settings.strength, 0.0, 1.0) * weight;
        for (const auto& [p, v] : members(group)) {
            if (patches_[p].value.disp.alphas[v] == alpha) continue;
            touch(p);
            patches_[p].value.disp.alphas[v] = alpha;
        }
    }
}

Result<void> DisplacementStroke::finish() {
    for (const auto p : moved_)
        if (auto tagged = retagDisplacement(patches_[p].value.corners, patches_[p].value.disp); !tagged) return tagged;
    return {};
}

void DisplacementStroke::revert() {
    for (auto& [p, disp] : originals_) {
        auto& patch = patches_[p];
        patch.value.disp = std::move(disp);
        const auto& d = patch.value.disp;
        for (std::size_t v = 0; v < patch.positions.size(); ++v)
            patch.positions[v] = patch.flat[v] + d.normals[v] * d.distances[v] + d.offsets[v];
        patch.normalsDirty = true;
        refreshBounds(p);
    }
    begin();
}

std::vector<std::size_t> DisplacementStroke::touched() const {
    std::vector<std::size_t> result;
    for (const auto& [p, disp] : originals_) result.push_back(p);
    return result;
}

Result<void> DisplacementStroke::reset(std::size_t p, Displacement disp) {
    if (p >= patches_.size()) return std::unexpected(invalid("no such displacement in the stroke"));
    auto& patch = patches_[p];
    if (disp.power != patch.value.disp.power) return std::unexpected(invalid("displacement power changed"));
    if (const auto problem = disp.problem()) return std::unexpected(invalid(*problem));
    patch.value.disp = std::move(disp);
    const auto& d = patch.value.disp;
    for (std::size_t v = 0; v < patch.positions.size(); ++v)
        patch.positions[v] = patch.flat[v] + d.normals[v] * d.distances[v] + d.offsets[v];
    patch.normalsDirty = true;
    refreshBounds(p);
    return {};
}

} // namespace apex::geometry
