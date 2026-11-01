#pragma once

#include <span>
#include <vector>

#include "render/gpu_lighting.hpp"
#include "render/texture_heap.hpp"
#include "render/view_math.hpp"

namespace apex::render {

inline constexpr std::uint32_t kShadowAtlasSize = 4096;
inline constexpr std::uint32_t kMinShadowTile = 256, kMaxShadowTile = 1024; // Powers of two (D13).
inline constexpr std::uint32_t kMaxShadowViews = 256; // One per minimum tile of the atlas.
inline constexpr std::uint32_t kSunCascades = 3;
inline constexpr float kShadowNormalOffset = 0.125f; // Map units at a 1024 tile; provisional (D13).
inline constexpr float kShadowConstantBias = -1.0f, kShadowSlopeBias = -1.5f; // Reversed-Z.
inline constexpr double kSunCascadeEnd[kSunCascades] = {320.0, 1280.0, 5120.0}; // View depth; provisional.
inline constexpr double kSunBack = 32768.0; // Eye distance behind a cascade, so casters up-sun are not clipped.

struct ShadowRect {
    std::uint32_t x = 0, y = 0, size = 0; // Atlas texels.
};

// One view into the atlas. params: x unused, y normal offset (map units) plus z per unit distance
// from the light (perspective tiles), both applied along the geometric normal.
struct alignas(16) GpuShadowTile {
    Mat4 projection{1.0f}; // Camera-relative world to light clip.
    Vec4 rect{0.0f}; // Atlas UV offset and scale.
    Vec4 params{0.0f};
};
static_assert(sizeof(GpuShadowTile) == 96);

inline Vec4 atlasRect(ShadowRect r) {
    const float scale = 1.0f / float(kShadowAtlasSize);
    return Vec4(float(r.x) * scale, float(r.y) * scale, float(r.size) * scale, float(r.size) * scale);
}

inline Mat4 lightView(DVec3 forward, DVec3 up, DVec3 origin, DVec3 camera) {
    const DVec3 right = glm::normalize(glm::cross(forward, up));
    return rotationOnlyView(forward, right, glm::cross(right, forward)) * cameraRelativeTranslation(camera, origin);
}

// Spotlights with a narrow cone use one perspective view; wider ones use the point light's six (D7).
inline bool spotUsesCube(const lighting::Light& light) { return light.stopdot2 < 0.25; }

inline GpuShadowTile spotShadowTile(const lighting::Light& light, DVec3 camera, ShadowRect rect) {
    const DVec3 forward = glm::normalize(light.direction);
    const DVec3 up = std::abs(forward.z) < 0.99 ? DVec3(0, 0, 1) : DVec3(0, 1, 0);
    const auto projection = reversedZPerspective(2 * std::acos(light.stopdot2), 1, 0.25);
    return {projection * lightView(forward, up, light.origin, camera), atlasRect(rect),
            Vec4(0, kShadowNormalOffset * float(kMaxShadowTile) / float(rect.size), 0, 0)};
}

// Face order +X -X +Y -Y +Z -Z; the shader selects by the major axis of the offset to the light.
// The face is rendered one texel wider on every side (a wider field of view), so a 3x3 filter taps real
// depth across the seam instead of clamping at the edge (D13).
inline GpuShadowTile pointShadowTile(const lighting::Light& light, DVec3 camera, std::uint32_t face, ShadowRect rect) {
    static constexpr double axes[6][2][3] = {{{1, 0, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 0, 1}}, {{0, 1, 0}, {0, 0, 1}},
                                             {{0, -1, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}}, {{0, 0, -1}, {1, 0, 0}}};
    const double n = double(rect.size);
    const auto projection = reversedZPerspective(2 * std::atan(n / (n - 2.0)), 1, 0.25);
    const auto& a = axes[face];
    return {projection * lightView(DVec3(a[0][0], a[0][1], a[0][2]), DVec3(a[1][0], a[1][1], a[1][2]), light.origin, camera),
            atlasRect(rect), Vec4(0, kShadowNormalOffset * float(kMaxShadowTile) / float(rect.size), 2.0f / float(rect.size), 0)};
}

// Cascades fit a bounding sphere of each view-depth slice (rotation-stable size) and snap its centre to the
// texel grid in world space, so a moving camera does not shimmer the edges. The camera's frustum is rebuilt
// from viewProjection (two depths per corner, so it also serves an infinite far plane and orthographic views).
// Returns fewer than kSunCascades when an orthographic far plane ends earlier.
inline std::vector<GpuShadowTile> sunCascadeTiles(DVec3 direction, const Mat4& viewProjection, DVec3 camera,
                                                  std::span<const ShadowRect> rects) {
    std::vector<GpuShadowTile> tiles;
    const DMat4 inverse = glm::inverse(DMat4(viewProjection));
    const auto unproject = [&](double x, double y, double z) {
        const DVec4 p = inverse * DVec4(x, y, z, 1.0);
        return DVec3(p) / p.w;
    };
    const DVec3 nearCentre = unproject(0, 0, 1), midCentre = unproject(0, 0, 0.5);
    const DVec3 axis = midCentre - nearCentre;
    if (!(glm::length(axis) > 0) || !(glm::length(direction) > 0)) return tiles;
    const DVec3 view = glm::normalize(axis);
    const double nearDepth = glm::dot(nearCentre, view);
    double farDepth = std::numeric_limits<double>::infinity();
    if (const DVec4 far = inverse * DVec4(0, 0, 0, 1); std::abs(far.w) > 1e-9) farDepth = glm::dot(DVec3(far) / far.w, view);
    const DVec3 forward = glm::normalize(direction);
    const DVec3 up = std::abs(forward.z) < 0.99 ? DVec3(0, 0, 1) : DVec3(0, 1, 0);
    const DVec3 right = glm::normalize(glm::cross(forward, up)), lightUp = glm::cross(right, forward);
    double start = nearDepth;
    for (std::size_t i = 0; i < rects.size() && i < kSunCascades && start < farDepth; ++i) {
        const double end = std::min(kSunCascadeEnd[i], farDepth);
        DVec3 corners[8];
        for (int c = 0; c < 4; ++c) {
            const double x = (c & 1) ? 1.0 : -1.0, y = (c & 2) ? 1.0 : -1.0;
            const DVec3 a = unproject(x, y, 1), b = unproject(x, y, 0.5);
            const double da = glm::dot(a, view), db = glm::dot(b, view);
            corners[c] = a + (b - a) * ((start - da) / (db - da));
            corners[c + 4] = a + (b - a) * ((end - da) / (db - da));
        }
        DVec3 centre(0.0);
        for (const auto& corner : corners) centre += corner / 8.0;
        double radius = 0;
        for (const auto& corner : corners) radius = std::max(radius, glm::length(corner - centre));
        radius = std::ceil(radius * 8.0) / 8.0;
        const double texel = 2.0 * radius / double(rects[i].size);
        const DVec3 world = centre + camera;
        const double x = glm::dot(world, right), y = glm::dot(world, lightUp);
        centre += right * (std::floor(x / texel + 0.5) * texel - x) + lightUp * (std::floor(y / texel + 0.5) * texel - y);
        const auto projection = reversedZOrtho(radius, radius, 0, kSunBack + radius);
        tiles.push_back({projection * rotationOnlyView(forward, right, lightUp) *
                             cameraRelativeTranslation(DVec3(0.0), centre - forward * kSunBack),
                         atlasRect(rects[i]), Vec4(0, 1.5f * float(texel), 0, 0)});
        start = end;
    }
    return tiles;
}

// One atlas per recording slot: another viewport may still be sampling its own (D5).
class ShadowAtlas {
  public:
    ShadowAtlas(Context& context, TextureHeap& heap) : context_(&context), heap_(&heap) {}
    ~ShadowAtlas() { heap_->releaseAttachment(slot_); } // Renderer waits for all slots first.
    Result<void> create() {
        auto image = Image::create(*context_, {VK_FORMAT_D32_SFLOAT, kShadowAtlasSize, kShadowAtlasSize, 1, 1,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
        if (!image) return std::unexpected(image.error());
        auto slot = heap_->registerAttachment(*image, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!slot) return std::unexpected(slot.error());
        image_ = std::move(*image);
        slot_ = *slot;
        return {};
    }
    TextureSlot slot() const { return slot_; }
    // `area` bounds every tile drawn this frame: only that region is cleared and rendered.
    void begin(VkCommandBuffer commands, VkRect2D area) {
        transitionImage(commands, image_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        auto depth = vkStruct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
        depth.imageView = image_.view();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {0.0f, 0}; // Reversed-Z.
        auto info = vkStruct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
        info.renderArea = area;
        info.layerCount = 1;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(commands, &info);
    }
    static void tile(VkCommandBuffer commands, ShadowRect rect) {
        const VkViewport viewport{float(rect.x), float(rect.y), float(rect.size), float(rect.size), 0, 1};
        const VkRect2D scissor{{std::int32_t(rect.x), std::int32_t(rect.y)}, {rect.size, rect.size}};
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
    }
    void end(VkCommandBuffer commands) {
        vkCmdEndRendering(commands);
        transitionImage(commands, image_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

  private:
    Context* context_;
    TextureHeap* heap_;
    Image image_;
    TextureSlot slot_ = kCheckerboardSlot;
};

} // namespace apex::render
