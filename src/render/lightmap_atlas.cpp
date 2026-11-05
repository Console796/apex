#include "render/lightmap_atlas.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <tuple>

namespace apex::render {
namespace {
std::unexpected<Error> fail(const char* message) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, message});
}
}

Result<LightmapAtlas> LightmapAtlas::pack(std::span<const lighting::BakeFace> input, int maxSize) {
    if (maxSize < 1 || maxSize > 16384) return fail("invalid lightmap atlas limit");
    LightmapAtlas atlas;
    atlas.faces.assign(input.begin(), input.end());
    atlas.rects.resize(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto dims = input[i].chart.dimensions;
        if (dims[0] < 1 || dims[1] < 1 || dims[0] > maxSize - 2 || dims[1] > maxSize - 2)
            return fail("lightmap rectangle exceeds atlas limit");
        atlas.fragments[{input[i].solidId, input[i].sideId}].push_back(i);
    }
    // Stable descending-height shelves; input index breaks ties deterministically.
    std::vector<std::size_t> order(input.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
        return input[a].chart.dimensions[1] > input[b].chart.dimensions[1];
    });
    for (int size = 1;; size = std::min(maxSize, size * 2)) {
        int x = 0, y = 0, shelf = 0;
        bool fits = true;
        for (const auto i : order) {
            const auto dims = input[i].chart.dimensions;
            const int w = dims[0] + 2, h = dims[1] + 2;
            if (x + w > size) { y += shelf; x = 0; shelf = 0; }
            if (w > size || y + h > size) { fits = false; break; }
            atlas.rects[i] = {x + 1, y + 1, dims[0], dims[1]};
            x += w;
            shelf = std::max(shelf, h);
        }
        if (fits) { atlas.size = size; return atlas; }
        if (size == maxSize) return fail("lightmaps do not fit in atlas");
    }
}

DVec2 LightmapAtlas::uv(std::size_t face, DVec3 world) const {
    const auto& chart = faces.at(face).chart;
    const auto& rect = rects.at(face);
    DVec2 local;
    for (int axis = 0; axis < 2; ++axis)
        local[axis] = glm::dot(world, chart.worldToLuxel[axis]) + chart.offset[axis] - chart.mins[axis];
    return (DVec2(rect.x, rect.y) + local + 0.5) / double(size);
}

void LightmapAtlas::bindInstances(std::span<const document::InstanceSolidOrigin> origins) {
    for (const auto& origin : origins) {
        bool baked = false;
        for (const auto& [side, expanded] : origin.sides) {
            if (!fragments.contains({origin.solid, expanded})) continue;
            baked = true;
            const TemplateSide identity{origin.placement, origin.templateSolid, side};
            templateOfSide[{origin.solid, expanded}] = identity;
            sideOfTemplate[identity] = {origin.solid, expanded};
        }
        if (baked) instances[origin.placement][origin.templateSolid] = origin;
    }
}

std::vector<lighting::FaceKey> LightmapAtlas::faceKeys() const {
    std::vector<lighting::FaceKey> keys;
    keys.reserve(faces.size());
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint32_t>, std::uint32_t> ordinal;
    for (const auto& face : faces) {
        lighting::FaceKey key{0, face.solidId, face.sideId, 0};
        if (const auto found = templateOfSide.find({face.solidId, face.sideId}); found != templateOfSide.end()) {
            std::uint64_t hash = (0xcbf29ce484222325ULL ^ found->second.placement.owner) * 0x100000001b3ULL;
            for (const auto id : found->second.placement.nested) hash = (hash ^ std::uint64_t(id)) * 0x100000001b3ULL;
            key = {hash | 1, std::uint64_t(found->second.solid), std::uint32_t(found->second.side), 0};
        }
        key.fragment = ordinal[{key.scope, key.solid, key.side}]++;
        keys.push_back(key);
    }
    return keys;
}

std::vector<std::vector<Vec3>> LightmapAtlas::retainRadiance(
    const LightmapAtlas* previous, const std::vector<std::vector<Vec3>>& radiance,
    std::vector<char>* covered) const {
    if (covered) covered->assign(faces.size(), 0);
    std::vector<std::vector<Vec3>> retained(faces.size());
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const auto& face = faces[i];
        const auto dims = face.chart.dimensions;
        retained[i].resize(std::size_t(dims[0]) * std::size_t(dims[1]), Vec3{0});
        if (!previous) continue;
        std::pair<std::uint64_t, std::uint32_t> key{face.solidId, face.sideId};
        // Expansion ids are renumbered by edits: instance faces follow their template identity.
        if (const auto own = templateOfSide.find(key); own != templateOfSide.end()) {
            const auto old = previous->sideOfTemplate.find(own->second);
            if (old == previous->sideOfTemplate.end()) continue;
            key = old->second;
        } else if (previous->templateOfSide.contains(key)) continue;
        const auto found = previous->fragments.find(key);
        if (found == previous->fragments.end()) continue;
        if (covered) (*covered)[i] = 1;
        for (int y = 0; y < dims[1]; ++y) for (int x = 0; x < dims[0]; ++x) {
            const auto world = face.chart.worldAt({x, y});
            double best = std::numeric_limits<double>::max();
            Vec3 value{0};
            for (const auto old : found->second) {
                const auto& chart = previous->faces[old].chart;
                DVec2 local;
                for (int axis = 0; axis < 2; ++axis)
                    local[axis] = glm::dot(world, chart.worldToLuxel[axis]) + chart.offset[axis] - chart.mins[axis];
                const int sx = int(std::round(std::clamp(local.x, 0.0, double(chart.dimensions[0] - 1))));
                const int sy = int(std::round(std::clamp(local.y, 0.0, double(chart.dimensions[1] - 1))));
                const auto delta = chart.worldAt({sx, sy}) - world;
                const auto distance = glm::dot(delta, delta);
                if (distance < best) {
                    best = distance;
                    value = radiance[old][std::size_t(sy * chart.dimensions[0] + sx)];
                }
            }
            retained[i][std::size_t(y * dims[0] + x)] = value;
        }
    }
    return retained;
}

BakedLightmaps::BakedLightmaps(Context& context, TextureHeap& heap, LightmapAtlas atlas)
    : context_(context), heap_(heap), atlas_(std::make_shared<const LightmapAtlas>(std::move(atlas))) {}

Result<std::shared_ptr<BakedLightmaps>> BakedLightmaps::create(Context& context, TextureHeap& heap,
                                                             LightmapAtlas atlas) {
    if (atlas.size < 1) return fail("unpacked lightmap atlas");
    auto maps = std::shared_ptr<BakedLightmaps>(new BakedLightmaps(context, heap, std::move(atlas)));
    auto image = Image::create(context, {VK_FORMAT_R16G16B16A16_SFLOAT,
        static_cast<std::uint32_t>(maps->atlas_->size), static_cast<std::uint32_t>(maps->atlas_->size), 1, 1,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
    if (!image) return std::unexpected(image.error());
    maps->image_ = std::move(*image);
    auto cleared = context.oneShot([&](VkCommandBuffer commands) {
        transitionImage(commands, maps->image_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(commands, maps->image_.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        transitionImage(commands, maps->image_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    if (!cleared) return std::unexpected(cleared.error());
    auto slot = heap.registerAttachment(maps->image_, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!slot) return std::unexpected(slot.error());
    maps->slot_ = *slot;
    return maps;
}

BakedLightmaps::~BakedLightmaps() {
    context_.waitIdle();
    if (slot_ != kNoTexture) heap_.releaseAttachment(slot_);
}

std::vector<std::vector<Vec3>> densityRadiance(const LightmapAtlas& atlas, double target) {
    std::vector<std::vector<Vec3>> result;
    result.reserve(atlas.faces.size());
    for (const auto& face : atlas.faces) {
        const auto& chart = face.chart;
        const double units = 0.5 * (1.0 / glm::length(chart.worldToLuxel[0]) + 1.0 / glm::length(chart.worldToLuxel[1]));
        const double ratio = units / target;
        const Vec3 hue = ratio > 1.1 ? Vec3(1.0f, 0.15f, 0.1f) : ratio < 0.9 ? Vec3(0.15f, 0.35f, 1.0f)
                                                                              : Vec3(0.15f, 1.0f, 0.2f);
        auto& luxels = result.emplace_back();
        const int width = chart.dimensions[0], height = chart.dimensions[1];
        luxels.reserve(std::size_t(width) * std::size_t(height));
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            luxels.push_back(hue * (((x + y) & 1) ? 1.0f : 0.4f));
    }
    return result;
}

std::string densityLegend(double target) {
    return "Luxel density: one checker cell per luxel; green " + std::to_string(int(target)) +
           " u/luxel (+-10%), red coarser, blue finer";
}

Result<void> BakedLightmaps::upload(const std::vector<std::vector<Vec3>>& radiance,
                                   std::span<const std::size_t> selected) {
    if (radiance.size() != atlas_->faces.size()) return fail("lightmap face count mismatch");
    std::vector<std::size_t> all;
    if (selected.empty()) {
        all.resize(radiance.size());
        std::iota(all.begin(), all.end(), 0);
        selected = all;
    }
    std::set<std::size_t> unique;
    for (const auto i : selected) {
        if (i >= atlas_->rects.size() || !unique.insert(i).second) return fail("invalid lightmap selection");
        const auto& rect = atlas_->rects[i];
        if (radiance[i].size() != std::size_t(rect.width) * std::size_t(rect.height))
            return fail("lightmap luxel count mismatch");
    }
    context_.waitIdle();
    auto& ring = heap_.uploadRing();
    for (const auto i : selected) {
        const auto& rect = atlas_->rects[i];
        std::vector<std::uint16_t> pixels(std::size_t(rect.width + 2) * std::size_t(rect.height + 2) * 4);
        for (int y = -1; y <= rect.height; ++y) for (int x = -1; x <= rect.width; ++x) {
            const auto value = radiance[i][std::size_t(std::clamp(y, 0, rect.height - 1) * rect.width +
                                                      std::clamp(x, 0, rect.width - 1))];
            const auto offset = std::size_t((y + 1) * (rect.width + 2) + x + 1) * 4;
            for (int c = 0; c < 3; ++c) pixels[offset + std::size_t(c)] = glm::packHalf1x16(value[c]);
            pixels[offset + 3] = glm::packHalf1x16(1.0f);
        }
        auto staged = ring.stage(std::as_bytes(std::span{pixels}));
        if (!staged) return std::unexpected(staged.error());
        auto commands = ring.commands();
        if (!commands) return std::unexpected(commands.error());
        transitionImage(*commands, image_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.bufferOffset = *staged;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {rect.x - 1, rect.y - 1, 0};
        copy.imageExtent = {static_cast<std::uint32_t>(rect.width + 2), static_cast<std::uint32_t>(rect.height + 2), 1};
        vkCmdCopyBufferToImage(*commands, ring.buffer(), image_.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transitionImage(*commands, image_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return ring.flush();
}

} // namespace apex::render
