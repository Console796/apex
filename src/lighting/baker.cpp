#include "lighting/baker.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
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

Baker::Baker(BakeScene scene, BakerOptions options) : scene_(std::move(scene)), options_(options) {
    if (options.threads == 0 || options.skyRays == 0)
        throw std::invalid_argument("Baker: threads and skyRays must be positive");
    for (const auto& face : scene_.faces) {
        const auto dims = face.chart.dimensions;
        if (dims[0] <= 0 || dims[1] <= 0 || !std::isfinite(glm::length(face.normal)) ||
            std::abs(glm::length(face.normal) - 1.0) > 1e-6)
            throw std::invalid_argument("Baker: invalid chart dimensions or face normal");
        const auto count = static_cast<std::size_t>(dims[0]) * static_cast<std::size_t>(dims[1]);
        buffers_.push_back({dims, std::vector<Vec3>(count, Vec3{0}), std::vector<std::uint64_t>(count, 0)});
    }
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
        const DVec3 point = face.chart.worldAt(coordinate + offsets[i]) + face.normal;
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

BakeStep Baker::step(std::size_t budget, std::span<const std::size_t> faces) {
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
    if (selected != selection_ || passComplete_) {
        selection_ = std::move(selected);
        nextFace_ = nextLuxel_ = 0;
        passComplete_ = false;
    }

    struct Work { std::size_t face, begin, end; };
    std::vector<Work> work;
    BakeStep result;
    auto nextFace = nextFace_, nextLuxel = nextLuxel_;
    while (nextFace < selection_.size() && result.luxels < budget) {
        const auto face = selection_[nextFace];
        const auto count = std::min({std::size_t{32}, buffers_[face].samples.size() - nextLuxel,
                                     budget - result.luxels});
        work.push_back({face, nextLuxel, nextLuxel + count});
        result.luxels += count;
        nextLuxel += count;
        if (nextLuxel == buffers_[face].samples.size()) {
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
                auto& buffer = buffers_[item.face];
                for (auto luxel = item.begin; luxel < item.end; ++luxel) {
                    const auto value = sample(item.face, luxel);
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
    nextFace_ = nextFace;
    nextLuxel_ = nextLuxel;
    result.passComplete = passComplete_ = nextFace_ == selection_.size();
    return result;
}

std::vector<std::vector<Vec3>> Baker::resolve() const {
    std::vector<std::vector<Vec3>> result;
    for (const auto& buffer : buffers_) {
        auto& face = result.emplace_back(buffer.radiance.size(), Vec3{0});
        for (std::size_t i = 0; i < face.size(); ++i)
            if (buffer.samples[i] > 0) face[i] = buffer.radiance[i] / static_cast<float>(buffer.samples[i]);
    }
    return result;
}

} // namespace apex::lighting
