#include "lighting/baker.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace apex::lighting {
namespace {

std::uint64_t mix(std::uint64_t value) {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

double unit(std::uint64_t key) {
    return static_cast<double>(mix(key) >> 11) * 0x1.0p-53;
}

} // namespace

DVec3 clampToFace(const BakeFace& face, DVec3 point) {
    constexpr double kInset = 1;
    const auto& vertices = face.vertices;
    if (vertices.size() < 3) return point;
    DVec3 centre{0};
    for (const auto& vertex : vertices) centre += vertex;
    centre /= static_cast<double>(vertices.size());
    bool inside = true;
    double best = std::numeric_limits<double>::infinity();
    DVec3 nearest = centre;
    for (std::size_t k = 0; k < vertices.size(); ++k) {
        const DVec3 a = vertices[k], edge = vertices[(k + 1) % vertices.size()] - a;
        const double length2 = glm::dot(edge, edge);
        if (length2 <= 0) continue;
        DVec3 inward = glm::cross(face.normal, edge);
        if (glm::dot(inward, centre - a) < 0) inward = -inward;
        if (glm::dot(inward, point - a) < kInset * std::sqrt(glm::dot(inward, inward))) inside = false;
        const DVec3 closest = a + edge * std::clamp(glm::dot(point - a, edge) / length2, 0.0, 1.0);
        if (const double distance = glm::length(point - closest); distance < best) {
            best = distance;
            nearest = closest;
        }
    }
    if (inside) return point;
    const DVec3 toCentre = centre - nearest;
    const double distance = glm::length(toCentre);
    return distance > 0 ? nearest + toCentre * std::min(1.0, kInset / distance) : centre;
}

Baker::Baker(BakeScene scene, BakerOptions options) : scene_(std::move(scene)), options_(options) {
    if (options.threads == 0 || options.skyRays == 0)
        throw std::invalid_argument("Baker: threads and skyRays must be positive");
    if (options.bounces > 0 && options.bounceRays == 0)
        throw std::invalid_argument("Baker: bounceRays must be positive");
    if (!(options.bounceReflectivity >= 0 && options.bounceReflectivity <= 1))
        throw std::invalid_argument("Baker: bounceReflectivity must be in [0,1]");
    for (const auto& face : scene_.faces) {
        const auto dims = face.chart.dimensions;
        if (dims[0] <= 0 || dims[1] <= 0 || !std::isfinite(glm::length(face.normal)) ||
            std::abs(glm::length(face.normal) - 1.0) > 1e-6)
            throw std::invalid_argument("Baker: invalid chart dimensions or face normal");
        if (options.bounces > 0)
            for (int c = 0; c < 3; ++c)
                if (!std::isfinite(face.albedo[c]) || face.albedo[c] < 0 || face.albedo[c] > 1)
                    throw std::invalid_argument("Baker: albedo must be finite and in [0,1]");
        const auto count = static_cast<std::size_t>(dims[0]) * static_cast<std::size_t>(dims[1]);
        buffers_.push_back({dims, std::vector<Vec3>(count, Vec3{0}), std::vector<std::uint64_t>(count, 0)});
    }
    bounceBuffers_.resize(options.bounces, buffers_);
    passes_.resize(static_cast<std::size_t>(options.bounces) + 1);
}

double Baker::skyVisibility(DVec3 point, DVec3 normal, std::uint64_t key) const {
    double visible = 0, open = 0;
    const double rotation = unit(key);
    for (unsigned i = 0; i < options_.skyRays; ++i) {
        const double z = (static_cast<double>(i) + unit(key ^ mix(i + 1))) / options_.skyRays;
        const double radius = std::sqrt(1 - z * z);
        const double phi = 2 * std::numbers::pi * (rotation + i * 0.6180339887498949);
        const DVec3 direction{radius * std::cos(phi), radius * std::sin(phi), z};
        const double weight = surfaceResponse(glm::dot(normal, direction));
        open += weight;
        if (weight > 0 && scene_.occlusion.reachesSky(point, direction)) visible += weight;
    }
    return open > 0 ? visible / open : 0;
}

DVec3 Baker::sample(std::size_t faceIndex, std::size_t luxel) const {
    const auto& face = scene_.faces[faceIndex];
    const auto count = buffers_[faceIndex].samples[luxel];
    const DVec2 coordinate{static_cast<double>(luxel % static_cast<std::size_t>(face.chart.dimensions[0])),
                           static_cast<double>(luxel / static_cast<std::size_t>(face.chart.dimensions[0]))};
    constexpr std::array<DVec2, 4> offsets{DVec2{-0.5, -0.5}, DVec2{0.5, -0.5},
                                         DVec2{-0.5, 0.5}, DVec2{0.5, 0.5}};
    DVec3 sum{0};
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        const DVec3 point = clampToFace(face, face.chart.worldAt(coordinate + offsets[i])) + face.normal;
        double sky = -1; // One visibility estimate shared by every ambient light at this point.
        for (const auto& light : scene_.lights) {
            if (!hasDirect(light)) continue;
            const DVec3 value = directIrradiance(light, point, face.normal);
            if (value == DVec3{0}) continue;
            if (light.kind == LightKind::Ambient) {
                if (sky < 0) sky = skyVisibility(point, face.normal,
                    mix(mix(mix(options_.seed) ^ faceIndex) ^ luxel) ^ mix(count + i + 1));
                sum += value * sky;
            } else if (light.kind == LightKind::Sun) {
                if (scene_.occlusion.reachesSky(point, -light.direction)) sum += value;
            } else if (scene_.occlusion.visible(point, light.origin + light.shadowCastOffset)) {
                sum += value;
            }
        }
    }
    return sum;
}

DVec3 Baker::sampleBounce(unsigned level, std::size_t faceIndex, std::size_t luxel) const {
    const auto& occlusion = scene_.occlusion;
    if (!occlusion.accel || !occlusion.faceOf) return DVec3{0};
    const auto& face = scene_.faces[faceIndex];
    const auto count = buffers(level)[faceIndex].samples[luxel];
    const auto& source = passes_[level].source;
    const DVec2 coordinate{static_cast<double>(luxel % static_cast<std::size_t>(face.chart.dimensions[0])),
                           static_cast<double>(luxel / static_cast<std::size_t>(face.chart.dimensions[0]))};
    const auto tangent = glm::normalize(glm::cross(
        std::abs(face.normal.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{0, 1, 0}, face.normal));
    const auto bitangent = glm::cross(face.normal, tangent);
    constexpr std::array<DVec2, 4> offsets{DVec2{-0.5, -0.5}, DVec2{0.5, -0.5},
                                         DVec2{-0.5, 0.5}, DVec2{0.5, 0.5}};
    DVec3 sum{0};
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        // Use the same float ray origin/direction to reconstruct the actual acceleration hit.
        const Vec3 point(clampToFace(face, face.chart.worldAt(coordinate + offsets[i])) + face.normal);
        const auto key = mix(mix(mix(mix(options_.seed) ^ faceIndex) ^ luxel) ^ level) ^ mix(count + i + 1);
        const double rotation = unit(key);
        for (unsigned ray = 0; ray < options_.bounceRays; ++ray) {
            const double u = (static_cast<double>(ray) + unit(key ^ mix(ray + 1))) / options_.bounceRays;
            const double radius = std::sqrt(u);
            const double phi = 2 * std::numbers::pi * (rotation + ray * 0.6180339887498949);
            const Vec3 direction(tangent * (radius * std::cos(phi)) +
                                 bitangent * (radius * std::sin(phi)) + face.normal * std::sqrt(1 - u));
            const auto hit = occlusion.accel->closest(point, direction, std::numeric_limits<float>::max());
            if (!hit || (occlusion.isSky && occlusion.isSky(*hit))) continue;
            const auto index = occlusion.faceOf(*hit);
            if (!index) continue;
            if (*index >= scene_.faces.size()) throw std::out_of_range("Baker: invalid faceOf index");
            const auto& hitFace = scene_.faces[*index];
            if (glm::dot(hitFace.normal, DVec3(direction)) >= 0) continue; // One-sided diffuse surface.
            const DVec3 position = DVec3(point) + DVec3(direction) * static_cast<double>(hit->distance);
            std::array<int, 2> st{};
            for (int axis = 0; axis < 2; ++axis) {
                const double local = glm::dot(position, hitFace.chart.worldToLuxel[axis]) +
                                     hitFace.chart.offset[axis] - hitFace.chart.mins[axis];
                st[axis] = static_cast<int>(std::round(std::clamp(local, 0.0,
                    static_cast<double>(hitFace.chart.dimensions[axis] - 1))));
            }
            const auto sample = static_cast<std::size_t>(st[1]) *
                                static_cast<std::size_t>(hitFace.chart.dimensions[0]) +
                                static_cast<std::size_t>(st[0]);
            // Lambertian albedo/pi and cosine/pi sampling cancel: a normalized mean of E * albedo.
            // Unlike the calibrated direct/sky surfaceResponse, this transport conserves energy.
            sum += DVec3(source[*index][sample]) * DVec3(hitFace.albedo) *
                   (options_.bounceReflectivity / static_cast<double>(options_.bounceRays));
        }
    }
    return sum;
}

BakeStep Baker::step(std::size_t budget, std::span<const std::size_t> faces) {
    return stepLevel(0, budget, faces);
}

BakeStep Baker::bounce(unsigned level, std::size_t budget, std::span<const std::size_t> faces) {
    if (level == 0 || level > options_.bounces) throw std::out_of_range("Baker: invalid bounce level");
    return stepLevel(level, budget, faces);
}

void Baker::adopt(unsigned level, std::size_t face, BakeBuffer buffer) {
    auto& target = level == 0 ? buffers_ : bounceBuffers_.at(level - 1);
    if (face >= target.size() || buffer.dimensions != target[face].dimensions ||
        buffer.radiance.size() != target[face].radiance.size() || buffer.samples.size() != target[face].samples.size())
        throw std::invalid_argument("Baker::adopt: buffer does not match the face");
    target[face] = std::move(buffer);
}

std::vector<BakeBuffer> Baker::releaseBuffers(unsigned level) {
    return std::move(level == 0 ? buffers_ : bounceBuffers_.at(level - 1));
}

const std::vector<BakeBuffer>& Baker::buffers(unsigned level) const {
    return level == 0 ? buffers_ : bounceBuffers_.at(level - 1);
}

BakeStep Baker::stepLevel(unsigned level, std::size_t budget, std::span<const std::size_t> faces) {
    auto& pass = passes_[level];
    auto& buffers = level == 0 ? buffers_ : bounceBuffers_[level - 1];
    std::vector<std::size_t> selected(faces.begin(), faces.end());
    if (faces.empty()) {
        selected.resize(scene_.faces.size());
        std::iota(selected.begin(), selected.end(), std::size_t{0});
    }
    auto sorted = selected;
    std::ranges::sort(sorted);
    if ((!sorted.empty() && sorted.back() >= scene_.faces.size()) ||
        std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        throw std::invalid_argument("Baker::step: invalid face selection");
    if (budget == 0) return {0, selected.empty()};
    if (selected != pass.selection || pass.complete) {
        if (level > 0) pass.source = resolveLevel(level - 1);
        pass.selection = std::move(selected);
        pass.nextFace = pass.nextLuxel = 0;
        pass.complete = false;
    }

    struct Work { std::size_t face, begin, end; };
    std::vector<Work> work;
    BakeStep result;
    auto nextFace = pass.nextFace, nextLuxel = pass.nextLuxel;
    while (nextFace < pass.selection.size() && result.luxels < budget) {
        const auto face = pass.selection[nextFace];
        const auto count = std::min({std::size_t{32}, buffers[face].samples.size() - nextLuxel,
                                     budget - result.luxels});
        work.push_back({face, nextLuxel, nextLuxel + count});
        result.luxels += count;
        nextLuxel += count;
        if (nextLuxel == buffers[face].samples.size()) {
            ++nextFace;
            nextLuxel = 0;
        }
    }
    std::atomic<std::size_t> next{0};
    std::exception_ptr error;
    std::mutex errorMutex;
    const auto run = [&] {
        try {
            for (;;) {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= work.size()) break;
                const auto& item = work[index];
                auto& buffer = buffers[item.face];
                for (auto luxel = item.begin; luxel < item.end; ++luxel) {
                    const auto value = level == 0 ? sample(item.face, luxel) : sampleBounce(level, item.face, luxel);
                    buffer.radiance[luxel] += Vec3(value);
                    buffer.samples[luxel] += 4;
                }
            }
        } catch (...) {
            std::lock_guard lock(errorMutex);
            error = std::current_exception();
        }
    };
    {
        std::vector<std::jthread> workers;
        const auto threads = std::min<std::size_t>(options_.threads, work.size());
        for (std::size_t i = 1; i < threads; ++i) workers.emplace_back(run);
        run();
    }
    if (error) std::rethrow_exception(error);
    pass.nextFace = nextFace;
    pass.nextLuxel = nextLuxel;
    result.passComplete = pass.complete = pass.nextFace == pass.selection.size();
    return result;
}

std::vector<std::vector<Vec3>> Baker::resolveLevel(unsigned level) const {
    std::vector<std::vector<Vec3>> result;
    for (const auto& buffer : buffers(level)) {
        auto& face = result.emplace_back(buffer.radiance.size(), Vec3{0});
        for (std::size_t i = 0; i < face.size(); ++i)
            if (buffer.samples[i] > 0) face[i] = buffer.radiance[i] / static_cast<float>(buffer.samples[i]);
    }
    return result;
}

std::vector<std::vector<Vec3>> Baker::resolve() const {
    auto result = resolveLevel(0);
    for (unsigned level = 1; level <= options_.bounces; ++level) {
        const auto bounce = resolveLevel(level);
        for (std::size_t face = 0; face < result.size(); ++face)
            for (std::size_t luxel = 0; luxel < result[face].size(); ++luxel)
                result[face][luxel] += bounce[face][luxel];
    }
    return result;
}

} // namespace apex::lighting
