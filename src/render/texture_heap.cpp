#include "render/texture_heap.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace apex::render {

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
    heap->capacity_ = std::max<std::uint32_t>(2, std::min(desc.capacity, limit));

    for (int i = 0; i < 2; ++i) {
        VkSamplerCreateInfo si = vkStruct<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        const bool nearest = i == 1;
        si.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        si.minFilter = si.magFilter;
        si.mipmapMode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.anisotropyEnable = !nearest && context.info().anisotropy ? VK_TRUE : VK_FALSE;
        si.maxAnisotropy = 16.0f;
        si.maxLod = VK_LOD_CLAMP_NONE;
        APEX_VK_TRY(vkCreateSampler(context.device(), &si, nullptr, &heap->samplers_[i]), "vkCreateSampler");
    }

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, heap->capacity_, VK_SHADER_STAGE_ALL, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 2, VK_SHADER_STAGE_ALL, heap->samplers_};
    const std::array<VkDescriptorBindingFlags, 2> flags{
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf = vkStruct<VkDescriptorSetLayoutBindingFlagsCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    bf.bindingCount = static_cast<std::uint32_t>(flags.size());
    bf.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo li = vkStruct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    li.pNext = &bf;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = static_cast<std::uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    APEX_VK_TRY(vkCreateDescriptorSetLayout(context.device(), &li, nullptr, &heap->layout_), "texture heap layout");

    const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, heap->capacity_},
                                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 2}}};
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
    return heap;
}

TextureHeap::~TextureHeap() {
    if (!context_) return;
    context_->waitIdle();
    ring_ = UploadRing();
    images_.clear();
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

Result<TextureSlot> TextureHeap::upload(const TextureDesc& desc, std::span<const std::byte> pixels) {
    const std::array<std::span<const std::byte>, 1> mips{pixels};
    return upload(desc, mips);
}

Result<TextureSlot> TextureHeap::upload(const TextureDesc& desc,
                                        std::span<const std::span<const std::byte>> mips) {
    if (desc.width == 0 || desc.height == 0 || mips.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "texture needs size and at least one mip"});
    }
    if (!formatInfo(desc.format)) {
        return std::unexpected(Error{ErrorCode::Unsupported, "unsupported texture format"});
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(context_->physicalDevice(), desc.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        return std::unexpected(Error{ErrorCode::Unsupported, "texture format not sampleable on this device"});
    }
    const auto levels = static_cast<std::uint32_t>(mips.size());
    for (std::uint32_t i = 0; i < levels; ++i) {
        const auto w = std::max(desc.width >> i, 1u), h = std::max(desc.height >> i, 1u);
        if (mips[i].size() != mipByteSize(desc.format, w, h)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         "mip " + std::to_string(i) + " has the wrong byte size"});
        }
    }
    if (freeSlots_.empty()) return std::unexpected(Error{ErrorCode::Internal, "texture heap is full"});

    auto image = Image::create(*context_, {desc.format, desc.width, desc.height, levels, 1,
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT});
    if (!image) return std::unexpected(image.error());

    // Copy offsets must be multiples of the block size and 4; 16 covers every supported format.
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
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        region.imageExtent = {std::max(desc.width >> i, 1u), std::max(desc.height >> i, 1u), 1};
        vkCmdCopyBufferToImage(*commands, ring_.buffer(), image->handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    auto commands = ring_.commands();
    if (!commands) return std::unexpected(commands.error());
    transitionImage(*commands, image->handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const TextureSlot slot = freeSlots_.back();
    freeSlots_.pop_back();
    images_[slot] = std::move(*image);
    writeDescriptor(slot, images_[slot].view());
    return slot;
}

Result<void> TextureHeap::flush() {
    return ring_.flush();
}

void TextureHeap::release(TextureSlot slot) {
    if (slot == kCheckerboardSlot || slot >= capacity_ || !images_[slot]) return;
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
        std::ranges::count_if(images_, [](const Image& image) { return static_cast<bool>(image); }));
}

} // namespace apex::render
