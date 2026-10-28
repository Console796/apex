#include "render/model_thumbnail.hpp"
#include "assets/model_pose.hpp"

#include "assets/thumbnail_cache.hpp"
#include "render/view_math.hpp"

#include <algorithm>
#include <array>

namespace apex::render {

ModelThumbnailRenderer::ModelThumbnailRenderer(Context& context, TextureHeap& heap, TextureSlot first,
                                               RenderTarget target, std::unique_ptr<Renderer> renderer)
    : context_(context), heap_(heap), first_(first), target_(std::move(target)), renderer_(std::move(renderer)) {}

Result<std::unique_ptr<ModelThumbnailRenderer>> ModelThumbnailRenderer::create(Context& context, TextureHeap& heap) {
    auto first = heap.reserveThumbnailSlots();
    if (!first) return std::unexpected(first.error());
    auto target = RenderTarget::create(context, assets::ThumbnailCache::sizeBucket(kSize), kSize);
    if (!target) return std::unexpected(target.error());
    auto renderer = Renderer::create(context, heap, RenderTarget::kColorFormat, RenderTarget::kDepthFormat);
    if (!renderer) return std::unexpected(renderer.error());
    return std::unique_ptr<ModelThumbnailRenderer>(new ModelThumbnailRenderer(
        context, heap, *first, std::move(*target), std::move(*renderer)));
}
ModelThumbnailRenderer::~ModelThumbnailRenderer() {
    // Every thumbnail submission and texture upload completes synchronously.
    for (const auto& texture : textures_) heap_.releaseThumbnail(texture.slot);
}
Result<TextureSlot> ModelThumbnailRenderer::texture(std::string_view name, const TextureDesc& desc,
                                                   std::span<const std::byte> pixels) {
    auto found = std::ranges::find(textures_, name, &Texture::name);
    if (found != textures_.end()) {
        const auto slot = found->slot;
        textures_.splice(textures_.end(), textures_, found);
        return slot;
    }
    const auto slot = textures_.size() < kThumbnailTextureSlots
        ? first_ + static_cast<TextureSlot>(textures_.size()) : textures_.front().slot;
    auto uploaded = heap_.uploadThumbnail(slot, desc, pixels);
    if (!uploaded) return std::unexpected(uploaded.error());
    if (textures_.size() == kThumbnailTextureSlots) textures_.pop_front();
    textures_.push_back({std::string(name), slot});
    return slot;
}
namespace {
struct RangeTexture {
    TextureSlot slot = 0;
    AlphaMode alpha = AlphaMode::Opaque;
};
}

Result<assets::Thumbnail> ModelThumbnailRenderer::render(const assets::ModelData& model,
                                                         const ModelMaterials& materials) {
    return draw(target_, model, materials, {}, true, kMaxTextureSize);
}
Result<assets::Thumbnail> ModelThumbnailRenderer::renderView(const assets::ModelData& model,
                                                             const ModelMaterials& materials, const ModelView& view) {
    if (!inspectTarget_.color()) {
        auto target = RenderTarget::create(context_, kInspectSize, kInspectSize);
        if (!target) return std::unexpected(target.error());
        inspectTarget_ = std::move(*target);
    }
    return draw(inspectTarget_, model, materials, view, false, kMaxInspectTextureSize);
}
Result<assets::Thumbnail> ModelThumbnailRenderer::draw(RenderTarget& target, const assets::ModelData& model,
                                                       const ModelMaterials& materials, const ModelView& view,
                                                       bool tight, std::uint32_t maxTexture) {
    const auto start = std::chrono::steady_clock::now();
    lastDuration_ = {};
    const auto ranges = assets::modelDrawRanges(model, view.body, view.skin);
    AABB bounds;
    for (const auto& range : ranges) {
        if (range.indexCount % 3 != 0 || std::uint64_t(range.firstIndex) + range.indexCount > model.indices.size())
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Invalid model triangle range"});
        for (std::uint64_t i = range.firstIndex; i < std::uint64_t(range.firstIndex) + range.indexCount; ++i) {
            const auto index = model.indices[i];
            if (index >= model.vertices.size())
                return std::unexpected(Error{ErrorCode::InvalidArgument, "Invalid model vertex index"});
            const DVec3 position(model.vertices[index].position);
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(position[axis]))
                    return std::unexpected(Error{ErrorCode::InvalidArgument, "Non-finite model position"});
            bounds.expand(position);
        }
    }
    if (bounds.empty()) return std::unexpected(Error{ErrorCode::InvalidArgument, "Model has no triangles"});
    if (view.sequence >= 0 && static_cast<std::size_t>(view.sequence) < model.sequences.size()) {
        const auto& sequence = model.sequences[static_cast<std::size_t>(view.sequence)];
        const auto& motion = sequence.bounds;
        if (sequence.supported && !motion.empty() &&
            std::isfinite(motion.min.x) && std::isfinite(motion.min.y) && std::isfinite(motion.min.z) &&
            std::isfinite(motion.max.x) && std::isfinite(motion.max.y) && std::isfinite(motion.max.z)) {
            bounds.expand(motion.min);
            bounds.expand(motion.max);
        }
    }
    const auto animated = assets::animateModel(model, view.sequence, view.frame);
    const auto& sourceVertices = animated.empty() ? model.vertices : animated;
    std::vector<BrushVertex> vertices;
    vertices.reserve(sourceVertices.size());
    for (const auto& v : sourceVertices)
        vertices.push_back({{v.position.x, v.position.y, v.position.z},
                            {v.normal.x, v.normal.y, v.normal.z}, {v.uv.x, v.uv.y}, 0});
    auto vb = uploadBuffer(context_, heap_.uploadRing(), std::as_bytes(std::span(vertices)),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    if (!vb) return std::unexpected(vb.error());
    auto ib = uploadBuffer(context_, heap_.uploadRing(), std::as_bytes(std::span(model.indices)),
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!ib) {
        (void)heap_.flush(); // Complete any vertex upload before its buffer is destroyed.
        return std::unexpected(ib.error());
    }
    const auto basis = angleVectors({std::clamp(view.pitch, -89.0, 89.0), view.yaw, 0.0});
    const double radius = std::max(glm::length(bounds.max - bounds.min) * 0.5, 0.01);
    const auto centre = (bounds.min + bounds.max) * 0.5 + (basis.right * view.pan.x + basis.up * view.pan.y) * radius;
    double halfSize = 0.01;
    if (!tight) halfSize = radius / std::clamp(view.zoom, 0.05, 64.0);
    for (int corner = 0; tight && corner < 8; ++corner) {
        const DVec3 p = DVec3{corner & 1 ? bounds.max.x : bounds.min.x,
                             corner & 2 ? bounds.max.y : bounds.min.y,
                             corner & 4 ? bounds.max.z : bounds.min.z} - centre;
        halfSize = std::max({halfSize, std::abs(glm::dot(p, basis.right)), std::abs(glm::dot(p, basis.up))});
    }
    // One mip per material, by material index; a material that cannot be uploaded stays flat.
    std::map<std::int32_t, RangeTexture> textured;
    for (const auto& range : ranges) {
        if (!range.indexCount || textured.contains(range.material)) continue;
        const auto found = materials.find(range.material);
        if (found == materials.end() || !found->second.material) continue;
        if (textured.size() + 1 >= kThumbnailTextureSlots) break; // One slot stays for the white fill.
        const assets::Texture& source = found->second.material->texture;
        const auto format = vulkanFormat(source.format);
        if (!format) continue;
        const auto mip = std::ranges::find_if(source.mips, [maxTexture](const assets::TextureMip& m) {
            return std::max(m.width, m.height) <= maxTexture;
        });
        if (mip == source.mips.end() || !mip->width || !mip->height) continue;
        const auto need = mipByteSize(*format, mip->width, mip->height);
        if (mip->data.size() < need) continue;
        const std::string key = found->second.name + (mip->width == source.mips[0].width ? "" : "@" + std::to_string(mip->width));
        auto slot = texture(key, {mip->width, mip->height, *format},
                             std::span<const std::byte>(mip->data.data(), static_cast<std::size_t>(need)));
        if (!slot) continue;
        const assets::Material& vmt = found->second.material->material;
        RangeTexture entry;
        entry.slot = *slot;
        entry.alpha = vmt.translucent ? AlphaMode::Blend : vmt.alphaTest ? AlphaMode::Test : AlphaMode::Opaque;
        textured[range.material] = entry;
    }
    TextureSlot white = kCheckerboardSlot;
    if (!textured.empty()) {
        static constexpr std::array<std::byte, 4> pixel{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        auto slot = texture("\x01white", {1, 1, VK_FORMAT_R8G8B8A8_UNORM}, pixel);
        if (slot) white = *slot; else textured.clear();
    }
    DrawList list;
    list.mode = textured.empty() ? DisplayMode::Flat : DisplayMode::Textured;
    list.view.cameraPosition = centre - basis.forward * (radius * 2.0);
    list.view.viewProjection = reversedZOrtho(halfSize * 1.1, halfSize * 1.1, radius * 0.5, radius * 3.5)
        * rotationOnlyView(basis);
    list.previews.push_back({DMat4(1.0), DVec3(0.0)});
    for (const auto& range : ranges) {
        if (!range.indexCount) continue;
        MeshDraw draw;
        draw.vertices = &*vb;
        draw.indices = &*ib;
        draw.firstIndex = range.firstIndex;
        draw.indexCount = range.indexCount;
        draw.colour = Vec4(0.65f, 0.72f, 0.8f, 1.0f);
        if (!textured.empty()) {
            const auto found = textured.find(range.material);
            draw.texture = found != textured.end() ? found->second.slot : white;
            if (found != textured.end()) {
                draw.colour = Vec4(1.0f);
                draw.alpha = found->second.alpha;
            }
        }
        draw.previewIndex = 1;
        draw.instancedTransforms = true;
        list.meshes.push_back(draw);
    }
    const float clear[] = {0.1f, 0.1f, 0.1f, 0.0f};
    auto submitted = context_.oneShot([&](VkCommandBuffer commands) {
        target.begin(commands, clear);
        renderer_->record(commands, list);
        target.end(commands);
    });
    if (!submitted) return std::unexpected(submitted.error());
    auto image = target.readback();
    lastDuration_ = std::chrono::steady_clock::now() - start;
    if (!image) return std::unexpected(image.error());
    return assets::Thumbnail{static_cast<int>(image->width), static_cast<int>(image->height), std::move(image->pixels)};
}

} // namespace apex::render
