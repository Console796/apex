#include "render/resources.hpp"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <utility>

namespace apex::render {

void Buffer::swap(Buffer& other) noexcept {
    std::swap(context_, other.context_);
    std::swap(buffer_, other.buffer_);
    std::swap(allocation_, other.allocation_);
    std::swap(size_, other.size_);
    std::swap(mapped_, other.mapped_);
    std::swap(address_, other.address_);
}

void Buffer::reset() {
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context_->allocator(), buffer_, allocation_);
    }
    context_ = nullptr;
    buffer_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    size_ = 0;
    mapped_ = nullptr;
    address_ = 0;
}

Result<Buffer> Buffer::create(Context& context, const BufferDesc& desc) {
    if (desc.size == 0 || desc.usage == 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "buffer needs a size and usage"});
    }
    VkBufferCreateInfo bi = vkStruct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bi.size = desc.size;
    bi.usage = desc.usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    switch (desc.memory) {
    case MemoryUsage::GpuOnly:
        ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case MemoryUsage::Upload:
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::Readback:
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }
    Buffer result;
    VmaAllocationInfo info{};
    APEX_VK_TRY(vmaCreateBuffer(context.allocator(), &bi, &ai, &result.buffer_, &result.allocation_, &info),
                "vmaCreateBuffer");
    result.context_ = &context;
    result.size_ = desc.size;
    result.mapped_ = info.pMappedData;
    if (desc.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo da = vkStruct<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
        da.buffer = result.buffer_;
        result.address_ = vkGetBufferDeviceAddress(context.device(), &da);
    }
    return result;
}

void Buffer::flush(VkDeviceSize offset, VkDeviceSize size) const {
    if (buffer_) vmaFlushAllocation(context_->allocator(), allocation_, offset, size);
}
void Buffer::invalidate(VkDeviceSize offset, VkDeviceSize size) const {
    if (buffer_) vmaInvalidateAllocation(context_->allocator(), allocation_, offset, size);
}

void Image::swap(Image& other) noexcept {
    std::swap(context_, other.context_);
    std::swap(image_, other.image_);
    std::swap(view_, other.view_);
    std::swap(allocation_, other.allocation_);
    std::swap(desc_, other.desc_);
}

void Image::reset() {
    if (view_ != VK_NULL_HANDLE) vkDestroyImageView(context_->device(), view_, nullptr);
    if (image_ != VK_NULL_HANDLE) vmaDestroyImage(context_->allocator(), image_, allocation_);
    context_ = nullptr;
    image_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    desc_ = {};
}

VkImageAspectFlags Image::aspect() const {
    return aspectOf(desc_.format);
}

Result<Image> Image::create(Context& context, const ImageDesc& desc) {
    if (desc.format == VK_FORMAT_UNDEFINED || desc.width == 0 || desc.height == 0 ||
        desc.mipLevels == 0 || desc.arrayLayers == 0 || desc.usage == 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid image description"});
    }
    VkImageCreateInfo ii = vkStruct<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = desc.format;
    ii.extent = {desc.width, desc.height, 1};
    ii.mipLevels = desc.mipLevels;
    ii.arrayLayers = desc.arrayLayers;
    ii.samples = desc.samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = desc.usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    Image result;
    APEX_VK_TRY(vmaCreateImage(context.allocator(), &ii, &ai, &result.image_, &result.allocation_, nullptr),
                "vmaCreateImage");
    result.context_ = &context;
    result.desc_ = desc;
    VkImageViewCreateInfo vi = vkStruct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    vi.image = result.image_;
    vi.viewType = desc.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = desc.format;
    vi.subresourceRange = {aspectOf(desc.format), 0, desc.mipLevels, 0, desc.arrayLayers};
    APEX_VK_TRY(vkCreateImageView(context.device(), &vi, nullptr, &result.view_), "vkCreateImageView");
    return result;
}

std::optional<FormatInfo> formatInfo(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
        return FormatInfo{1, 1, 1, false};
    case VK_FORMAT_R8G8_UNORM:
        return FormatInfo{1, 1, 2, false};
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return FormatInfo{1, 1, 4, false};
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return FormatInfo{1, 1, 8, false};
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
        return FormatInfo{4, 4, 8, true};
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return FormatInfo{4, 4, 16, true};
    default:
        return std::nullopt;
    }
}

VkDeviceSize mipByteSize(VkFormat format, std::uint32_t width, std::uint32_t height) {
    const auto info = formatInfo(format);
    if (!info) return 0;
    const VkDeviceSize blocksX = (width + info->blockWidth - 1) / info->blockWidth;
    const VkDeviceSize blocksY = (height + info->blockHeight - 1) / info->blockHeight;
    return blocksX * blocksY * info->blockBytes;
}

VkImageAspectFlags aspectOf(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

void transitionImage(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect,
                     VkImageLayout from, VkImageLayout to, std::uint32_t baseMip,
                     std::uint32_t mipCount, std::uint32_t layerCount) {
    VkImageMemoryBarrier2 barrier = vkStruct<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, baseMip, mipCount, 0, layerCount};
    VkDependencyInfo dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dep);
}

} // namespace apex::render
