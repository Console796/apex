#include "render/texture_heap.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <utility>
#include <glm/gtc/packing.hpp>

namespace apex::render {

std::optional<VkFormat> vulkanFormat(assets::TextureFormat format) {
    using F = assets::TextureFormat;
    switch (format) {
    case F::Rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
    case F::Bc1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case F::Bc2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case F::Bc3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case F::Bc4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case F::Bc5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case F::Bc6Signed: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case F::Bc6Unsigned: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case F::Bc7: return VK_FORMAT_BC7_UNORM_BLOCK;
    case F::Rgba32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return std::nullopt;
}

namespace {

constexpr std::uint32_t kCheckerSize = 64;
constexpr std::uint32_t kCheckerCell = 8;

std::vector<std::byte> checkerboardPixels() {
    std::vector<std::byte> pixels(kCheckerSize * kCheckerSize * 4);
    for (std::uint32_t y = 0; y < kCheckerSize; ++y) {
        for (std::uint32_t x = 0; x < kCheckerSize; ++x) {
            const bool magenta = ((x / kCheckerCell) + (y / kCheckerCell)) % 2 == 0;
            auto* p = &pixels[(y * kCheckerSize + x) * 4];
            p[0] = std::byte{magenta ? std::uint8_t{255} : std::uint8_t{0}};
            p[1] = std::byte{0};
            p[2] = std::byte{magenta ? std::uint8_t{255} : std::uint8_t{0}};
            p[3] = std::byte{255};
        }
    }
    return pixels;
}

// Decoded texture data as tightly packed mips of `layers` consecutive frame-0 faces.
struct PackedTexture {
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<assets::Blob> owned;
    std::vector<std::span<const std::byte>> mips;
};

std::optional<PackedTexture> packTexture(const assets::Texture& texture, std::uint32_t layers) {
    const auto format = vulkanFormat(texture.format);
    if (!format || texture.mips.empty()) return std::nullopt;
    PackedTexture packed;
    const bool floats = texture.format == assets::TextureFormat::Rgba32Float;
    packed.format = floats ? VK_FORMAT_R16G16B16A16_SFLOAT : *format;
    if (floats) {
        for (const auto& mip : texture.mips) {
            const auto count = std::size_t(mip.width) * mip.height * 4 * layers;
            if (mip.data.size() < count * sizeof(float)) break;
            assets::Blob data(count * sizeof(std::uint16_t));
            for (std::size_t i = 0; i < count; ++i) {
                float value;
                std::memcpy(&value, mip.data.data() + i * sizeof(float), sizeof(value));
                const auto half = glm::packHalf1x16(value);
                std::memcpy(data.data() + i * sizeof(half), &half, sizeof(half));
            }
            packed.owned.push_back(std::move(data));
        }
        for (const auto& mip : packed.owned) packed.mips.emplace_back(mip);
    } else {
        for (const auto& mip : texture.mips) {
            const auto size = mipByteSize(*format, mip.width, mip.height) * layers;
            if (mip.data.size() < size) break;
            packed.mips.emplace_back(mip.data.data(), size);
        }
    }
    return packed;
}

// Gamma-encoded colour formats sample linear through their _SRGB view.
VkFormat srgbVariant(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
    default: return format;
    }
}

} // namespace

Result<std::unique_ptr<TextureHeap>> TextureHeap::create(Context& context, const TextureHeapDesc& desc) {
    std::unique_ptr<TextureHeap> heap(new TextureHeap());
    heap->context_ = &context;

    VkPhysicalDeviceVulkan12Properties p12 = vkStruct<VkPhysicalDeviceVulkan12Properties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES);
    VkPhysicalDeviceProperties2 p2 = vkStruct<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    p2.pNext = &p12;
    vkGetPhysicalDeviceProperties2(context.physicalDevice(), &p2);
    const std::uint32_t limit = std::min(p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                         p12.maxDescriptorSetUpdateAfterBindSampledImages);
    heap->cubeCapacity_ = std::max<std::uint32_t>(1, desc.cubeCapacity);
    // Both bindings count against the per-stage sampled-image limit.
    heap->capacity_ = std::max<std::uint32_t>(2, std::min(desc.capacity,
        limit > heap->cubeCapacity_ ? limit - heap->cubeCapacity_ : 0u));

    for (int i = 0; i < 3; ++i) {
        VkSamplerCreateInfo si = vkStruct<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        const bool nearest = i == 1;
        const bool comparison = i == 2;
        si.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        si.minFilter = si.magFilter;
        si.mipmapMode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = comparison ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.compareEnable = comparison ? VK_TRUE : VK_FALSE;
        si.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        si.anisotropyEnable = !nearest && !comparison && context.info().anisotropy ? VK_TRUE : VK_FALSE;
        si.maxAnisotropy = 16.0f;
        si.maxLod = VK_LOD_CLAMP_NONE;
        APEX_VK_TRY(vkCreateSampler(context.device(), &si, nullptr, &heap->samplers_[i]), "vkCreateSampler");
    }

    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, heap->capacity_, VK_SHADER_STAGE_ALL, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 3, VK_SHADER_STAGE_ALL, heap->samplers_};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, heap->cubeCapacity_, VK_SHADER_STAGE_ALL, nullptr};
    const std::array<VkDescriptorBindingFlags, 3> flags{
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf = vkStruct<VkDescriptorSetLayoutBindingFlagsCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    bf.bindingCount = static_cast<std::uint32_t>(flags.size());
    bf.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo li = vkStruct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    li.pNext = &bf;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = static_cast<std::uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    APEX_VK_TRY(vkCreateDescriptorSetLayout(context.device(), &li, nullptr, &heap->layout_), "texture heap layout");

    const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, heap->capacity_ + heap->cubeCapacity_},
                                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 3}}};
    VkDescriptorPoolCreateInfo pi = vkStruct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pi.pPoolSizes = sizes.data();
    APEX_VK_TRY(vkCreateDescriptorPool(context.device(), &pi, nullptr, &heap->pool_), "texture heap pool");
    VkDescriptorSetAllocateInfo ai = vkStruct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    ai.descriptorPool = heap->pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &heap->layout_;
    APEX_VK_TRY(vkAllocateDescriptorSets(context.device(), &ai, &heap->set_), "texture heap set");

    auto ring = UploadRing::create(context, desc.stagingBytes);
    if (!ring) return std::unexpected(ring.error());
    heap->ring_ = std::move(*ring);

    heap->images_.resize(heap->capacity_);
    heap->attachments_.resize(heap->capacity_);
    for (TextureSlot slot = heap->capacity_; slot-- > 1;) heap->freeSlots_.push_back(slot);

    // Slot 0: checkerboard. Upload it directly into slot 0, then point every slot at it.
    const auto checker = checkerboardPixels();
    auto image = Image::create(context, {VK_FORMAT_R8G8B8A8_UNORM, kCheckerSize, kCheckerSize, 1, 1,
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT});
    if (!image) return std::unexpected(image.error().withContext("checkerboard"));
    heap->images_[0] = std::move(*image);
    auto offset = heap->ring_.stage(checker);
    if (!offset) return std::unexpected(offset.error());
    auto commands = heap->ring_.commands();
    if (!commands) return std::unexpected(commands.error());
    const Image& slot0 = heap->images_[0];
    transitionImage(*commands, slot0.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = *offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kCheckerSize, kCheckerSize, 1};
    vkCmdCopyBufferToImage(*commands, heap->ring_.buffer(), slot0.handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionImage(*commands, slot0.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (auto flushed = heap->ring_.flush(); !flushed) return std::unexpected(flushed.error());

    const VkDescriptorImageInfo info{VK_NULL_HANDLE, slot0.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::vector<VkDescriptorImageInfo> all(heap->capacity_, info);
    VkWriteDescriptorSet write = vkStruct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstSet = heap->set_;
    write.dstBinding = 0;
    write.descriptorCount = heap->capacity_;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = all.data();
    vkUpdateDescriptorSets(context.device(), 1, &write, 0, nullptr);

    // Cube slot 0: mid-grey until setDefaultCube(); every cube slot starts pointing at it.
    heap->cubes_.resize(heap->cubeCapacity_);
    for (TextureSlot slot = heap->cubeCapacity_; slot-- > 1;) heap->freeCubes_.push_back(slot);
    const std::array<std::byte, 24> grey = [] {
        std::array<std::byte, 24> pixels{};
        for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = std::byte{i % 4 == 3 ? std::uint8_t{255} : std::uint8_t{128}};
        return pixels;
    }();
    const std::array<std::span<const std::byte>, 1> greyMips{grey};
    auto cube = heap->createImage({1, 1, VK_FORMAT_R8G8B8A8_UNORM}, greyMips, true);
    if (!cube) return std::unexpected(cube.error().withContext("default cube"));
    if (auto flushed = heap->ring_.flush(); !flushed) return std::unexpected(flushed.error());
    heap->cubes_[0] = std::move(*cube);
    for (TextureSlot slot = 0; slot < heap->cubeCapacity_; ++slot) heap->writeCubeDescriptor(slot, heap->cubes_[0].view());
    return heap;
}

TextureHeap::~TextureHeap() {
    if (!context_) return;
    context_->waitIdle();
    ring_ = UploadRing();
    for (VkImageView view : attachments_)
        if (view) vkDestroyImageView(context_->device(), view, nullptr);
    images_.clear();
    cubes_.clear();
    garbage_.clear();
    VkDevice device = context_->device();
    if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
    if (layout_) vkDestroyDescriptorSetLayout(device, layout_, nullptr);
    for (VkSampler s : samplers_) {
        if (s) vkDestroySampler(device, s, nullptr);
    }
}

void TextureHeap::writeDescriptor(TextureSlot slot, VkImageView view) {
    const VkDescriptorImageInfo info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = vkStruct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstSet = set_;
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(context_->device(), 1, &write, 0, nullptr);
}

void TextureHeap::writeCubeDescriptor(TextureSlot slot, VkImageView view) {
    const VkDescriptorImageInfo info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = vkStruct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstSet = set_;
    write.dstBinding = 2;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(context_->device(), 1, &write, 0, nullptr);
}

Result<TextureSlot> TextureHeap::registerAttachment(const Image& image, VkImageAspectFlags aspect) {
    if (!image || !(image.desc().usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        (aspect != VK_IMAGE_ASPECT_COLOR_BIT && aspect != VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !(image.aspect() & aspect) || image.desc().arrayLayers != 1 || image.desc().samples != VK_SAMPLE_COUNT_1_BIT) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid sampled attachment"});
    }
    if (freeSlots_.empty()) return std::unexpected(Error{ErrorCode::Internal, "texture heap is full"});
    VkImageViewCreateInfo info = vkStruct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    info.image = image.handle();
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = image.desc().format;
    info.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    APEX_VK_TRY(vkCreateImageView(context_->device(), &info, nullptr, &view), "attachment sampled view");
    const TextureSlot slot = freeSlots_.back();
    freeSlots_.pop_back();
    attachments_[slot] = view;
    writeDescriptor(slot, view);
    return slot;
}

void TextureHeap::releaseAttachment(TextureSlot slot) {
    if (slot >= capacity_ || !attachments_[slot]) return;
    // Unlike uploaded textures, attachment retirement is the owner's fence/idle responsibility.
    writeDescriptor(slot, images_[0].view());
    vkDestroyImageView(context_->device(), attachments_[slot], nullptr);
    attachments_[slot] = VK_NULL_HANDLE;
    freeSlots_.push_back(slot);
}

void TextureHeap::setMaterial(TextureSlot slot, GpuMaterial material) {
    if (slot == kCheckerboardSlot || slot >= capacity_) return;
    if (materials_.size() <= slot) materials_.resize(slot + 1);
    materials_[slot] = material;
    ++materialRevision_;
}

Result<TextureSlot> TextureHeap::upload(const assets::Texture& texture) {
    const auto packed = packTexture(texture, 1);
    if (!packed) return std::unexpected(Error{ErrorCode::Unsupported, "Empty or unsupported texture"});
    return upload({texture.mips[0].width, texture.mips[0].height, packed->format}, packed->mips);
}

Result<TextureSlot> TextureHeap::upload(const TextureDesc& desc, std::span<const std::byte> pixels) {
    const std::array<std::span<const std::byte>, 1> mips{pixels};
    return upload(desc, mips);
}

Result<TextureSlot> TextureHeap::upload(const TextureDesc& desc,
                                        std::span<const std::span<const std::byte>> mips) {
    if (freeSlots_.empty()) return std::unexpected(Error{ErrorCode::Internal, "texture heap is full"});
    const auto slot = freeSlots_.back();
    auto result = uploadAt(slot, desc, mips);
    if (result) freeSlots_.pop_back();
    return result;
}

Result<TextureSlot> TextureHeap::uploadAt(TextureSlot slot, const TextureDesc& desc,
                                         std::span<const std::span<const std::byte>> mips) {
    auto image = createImage(desc, mips, false);
    if (!image) return std::unexpected(image.error());
    images_[slot] = std::move(*image);
    writeDescriptor(slot, images_[slot].view());
    return slot;
}

Result<Image> TextureHeap::createImage(const TextureDesc& desc, std::span<const std::span<const std::byte>> mips,
                                       bool cube) {
    if (desc.width == 0 || desc.height == 0 || mips.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "texture needs size and at least one mip"});
    }
    if (cube && desc.width != desc.height) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "cube faces must be square"});
    }
    if (!formatInfo(desc.format)) {
        return std::unexpected(Error{ErrorCode::Unsupported, "unsupported texture format"});
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(context_->physicalDevice(), desc.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        return std::unexpected(Error{ErrorCode::Unsupported, "texture format not sampleable on this device"});
    }
    const std::uint32_t layers = cube ? 6 : 1;
    const auto levels = static_cast<std::uint32_t>(mips.size());
    for (std::uint32_t i = 0; i < levels; ++i) {
        const auto w = std::max(desc.width >> i, 1u), h = std::max(desc.height >> i, 1u);
        if (mips[i].size() != mipByteSize(desc.format, w, h) * layers) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         "mip " + std::to_string(i) + " has the wrong byte size"});
        }
    }
    ImageDesc imageDesc{desc.format, desc.width, desc.height, levels, layers,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    imageDesc.cube = cube;
    auto image = Image::create(*context_, imageDesc);
    if (!image) return std::unexpected(image.error());

    // Copy offsets must be multiples of the block size and 4; 16 covers every supported format.
    // A cube mip's six faces are consecutive in the buffer, one copy covering every layer.
    for (std::uint32_t i = 0; i < levels; ++i) {
        auto offset = ring_.stage(mips[i], 16);
        if (!offset) return std::unexpected(offset.error());
        auto commands = ring_.commands();
        if (!commands) return std::unexpected(commands.error());
        if (i == 0) {
            transitionImage(*commands, image->handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        VkBufferImageCopy region{};
        region.bufferOffset = *offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, layers};
        region.imageExtent = {std::max(desc.width >> i, 1u), std::max(desc.height >> i, 1u), 1};
        vkCmdCopyBufferToImage(*commands, ring_.buffer(), image->handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    auto commands = ring_.commands();
    if (!commands) return std::unexpected(commands.error());
    transitionImage(*commands, image->handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return image;
}

Result<Image> TextureHeap::createCube(const assets::Texture& texture) {
    if (texture.faces < 6 || texture.mips.empty() || texture.mips[0].width != texture.mips[0].height)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "not a cube texture"});
    const auto packed = packTexture(texture, 6);
    if (!packed) return std::unexpected(Error{ErrorCode::Unsupported, "Empty or unsupported texture"});
    return createImage({texture.mips[0].width, texture.mips[0].height, srgbVariant(packed->format)},
                       packed->mips, true);
}

Result<TextureSlot> TextureHeap::uploadCube(const TextureDesc& desc, std::span<const std::span<const std::byte>> mips) {
    if (freeCubes_.empty()) return std::unexpected(Error{ErrorCode::Internal, "cube heap is full"});
    auto image = createImage(desc, mips, true);
    if (!image) return std::unexpected(image.error());
    const auto slot = freeCubes_.back();
    freeCubes_.pop_back();
    cubes_[slot] = std::move(*image);
    writeCubeDescriptor(slot, cubes_[slot].view());
    return slot;
}

Result<TextureSlot> TextureHeap::uploadCube(const assets::Texture& texture) {
    if (freeCubes_.empty()) return std::unexpected(Error{ErrorCode::Internal, "cube heap is full"});
    auto image = createCube(texture);
    if (!image) return std::unexpected(image.error());
    const auto slot = freeCubes_.back();
    freeCubes_.pop_back();
    cubes_[slot] = std::move(*image);
    writeCubeDescriptor(slot, cubes_[slot].view());
    return slot;
}

Result<void> TextureHeap::setDefaultCube(const assets::Texture& texture) {
    auto image = createCube(texture);
    if (!image) return std::unexpected(image.error());
    if (auto flushed = ring_.flush(); !flushed) return flushed;
    // Free slots alias slot 0's view, so no in-flight frame may still sample the old one.
    context_->waitIdle();
    garbage_.push_back(std::move(cubes_[kDefaultCubeSlot]));
    cubes_[kDefaultCubeSlot] = std::move(*image);
    for (TextureSlot slot = 0; slot < cubeCapacity_; ++slot)
        if (slot == kDefaultCubeSlot || !cubes_[slot]) writeCubeDescriptor(slot, cubes_[kDefaultCubeSlot].view());
    defaultCubeLoaded_ = true;
    return {};
}

void TextureHeap::releaseCube(TextureSlot slot) {
    if (slot == kDefaultCubeSlot || slot >= cubeCapacity_ || !cubes_[slot]) return;
    writeCubeDescriptor(slot, cubes_[kDefaultCubeSlot].view());
    garbage_.push_back(std::move(cubes_[slot]));
    cubes_[slot] = Image();
    freeCubes_.push_back(slot);
}

std::uint32_t TextureHeap::liveCubes() const {
    return static_cast<std::uint32_t>(std::ranges::count_if(cubes_, [](const Image& image) { return static_cast<bool>(image); }));
}

bool TextureHeap::thumbnailSlot(TextureSlot slot) const {
    return thumbnailFirst_ != 0 && slot >= thumbnailFirst_ && slot < thumbnailFirst_ + kThumbnailTextureSlots;
}
Result<TextureSlot> TextureHeap::reserveThumbnailSlots() {
    if (thumbnailFirst_) return thumbnailFirst_;
    // High slots keep the map's existing low-slot allocation order intact.
    if (capacity_ > kThumbnailTextureSlots) {
        for (TextureSlot first = capacity_ - kThumbnailTextureSlots; first > 0; --first) {
            bool free = true;
            for (auto slot = first; slot < first + kThumbnailTextureSlots; ++slot)
                if (images_[slot] || attachments_[slot]) { free = false; break; }
            if (!free) continue;
            thumbnailFirst_ = first;
            std::erase_if(freeSlots_, [&](TextureSlot slot) { return thumbnailSlot(slot); });
            return first;
        }
    }
    return std::unexpected(Error{ErrorCode::Internal, "No 64-slot thumbnail range available"});
}
Result<void> TextureHeap::uploadThumbnail(TextureSlot slot, const TextureDesc& desc, std::span<const std::byte> pixels) {
    if (!thumbnailSlot(slot))
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Not a thumbnail texture slot"});
    // Uploads must finish before replacing an image, even if no thumbnail sampled it yet.
    auto flushed = flush();
    if (!flushed) return flushed;
    const std::array<std::span<const std::byte>, 1> mips{pixels};
    auto result = uploadAt(slot, desc, mips);
    if (!result) return std::unexpected(result.error());
    maxThumbnailSlotsLive_ = std::max(maxThumbnailSlotsLive_, thumbnailSlotsLive());
    assert(maxThumbnailSlotsLive_ <= kThumbnailTextureSlots);
    return flush();
}
void TextureHeap::releaseThumbnail(TextureSlot slot) {
    if (!thumbnailSlot(slot) || !images_[slot]) return;
    writeDescriptor(slot, images_[0].view());
    images_[slot] = Image();
}
std::uint32_t TextureHeap::thumbnailSlotsLive() const {
    std::uint32_t count = 0;
    if (thumbnailFirst_)
        for (auto slot = thumbnailFirst_; slot < thumbnailFirst_ + kThumbnailTextureSlots; ++slot)
            if (images_[slot]) ++count;
    return count;
}

Result<void> TextureHeap::flush() {
    return ring_.flush();
}

void TextureHeap::release(TextureSlot slot) {
    if (slot == kCheckerboardSlot || slot >= capacity_ || thumbnailSlot(slot) || !images_[slot]) return;
    if (slot < materials_.size()) setMaterial(slot, {});
    writeDescriptor(slot, images_[kCheckerboardSlot].view());
    garbage_.push_back(std::move(images_[slot]));
    images_[slot] = Image();
    freeSlots_.push_back(slot);
}

void TextureHeap::collectGarbage() {
    // Pending uploads may still reference queued images, so submit them first.
    (void)ring_.flush();
    garbage_.clear();
}

std::uint32_t TextureHeap::liveTextures() const {
    return static_cast<std::uint32_t>(
        std::ranges::count_if(images_, [](const Image& image) { return static_cast<bool>(image); }) +
        std::ranges::count_if(attachments_, [](VkImageView view) { return view != VK_NULL_HANDLE; }));
}

} // namespace apex::render
