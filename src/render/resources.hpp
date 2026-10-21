#pragma once

// RAII wrappers for VMA-backed buffers and images, plus format and barrier helpers (plan 0.1,
// D13). Resources borrow the Context and must be destroyed before it.

#include "core/result.hpp"
#include "render/context.hpp"
#include "render/vk.hpp"

#include <cstdint>
#include <optional>

struct VmaAllocation_T;

namespace apex::render {

enum class MemoryUsage {
    GpuOnly,  // Device-local, not mappable.
    Upload,   // Host-visible, persistently mapped, sequential writes (staging, per-frame data).
    Readback, // Host-visible, persistently mapped, random-access reads (screenshots, tests).
};

struct BufferDesc {
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0; // Include VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT for addresses.
    MemoryUsage memory = MemoryUsage::GpuOnly;
};

class Buffer {
  public:
    Buffer() = default;
    ~Buffer() { reset(); }
    Buffer(Buffer&& other) noexcept { swap(other); }
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            reset();
            swap(other);
        }
        return *this;
    }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    static Result<Buffer> create(Context& context, const BufferDesc& desc);

    explicit operator bool() const { return buffer_ != VK_NULL_HANDLE; }
    VkBuffer handle() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    // Non-null for Upload and Readback buffers.
    void* mapped() const { return mapped_; }
    // 0 unless created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
    VkDeviceAddress deviceAddress() const { return address_; }
    // Needed only for non-coherent memory; harmless otherwise.
    void flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;
    void invalidate(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;
    void reset();

  private:
    void swap(Buffer& other) noexcept;
    Context* context_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation_T* allocation_ = nullptr;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;
    VkDeviceAddress address_ = 0;
};

struct ImageDesc {
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t mipLevels = 1;
    std::uint32_t arrayLayers = 1; // > 1 creates a 2D array view.
    VkImageUsageFlags usage = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// Device-local 2D image with a full-range view (aspect from the format). Layout tracking is the
// caller's job; the image starts in VK_IMAGE_LAYOUT_UNDEFINED.
class Image {
  public:
    Image() = default;
    ~Image() { reset(); }
    Image(Image&& other) noexcept { swap(other); }
    Image& operator=(Image&& other) noexcept {
        if (this != &other) {
            reset();
            swap(other);
        }
        return *this;
    }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    static Result<Image> create(Context& context, const ImageDesc& desc);

    explicit operator bool() const { return image_ != VK_NULL_HANDLE; }
    VkImage handle() const { return image_; }
    VkImageView view() const { return view_; }
    const ImageDesc& desc() const { return desc_; }
    VkExtent2D extent() const { return {desc_.width, desc_.height}; }
    VkImageAspectFlags aspect() const;
    void reset();

  private:
    void swap(Image& other) noexcept;
    Context* context_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VmaAllocation_T* allocation_ = nullptr;
    ImageDesc desc_;
};

struct FormatInfo {
    std::uint32_t blockWidth = 1; // Texels per block edge (4 for BC, 1 otherwise).
    std::uint32_t blockHeight = 1;
    std::uint32_t blockBytes = 0;
    bool compressed = false;
};

// Formats the engine uploads: R8, RG8, RGBA8/BGRA8 (UNORM/SRGB), RGBA16F, BC1-BC7.
std::optional<FormatInfo> formatInfo(VkFormat format);
// Bytes of one tightly packed mip level; 0 for unsupported formats.
VkDeviceSize mipByteSize(VkFormat format, std::uint32_t width, std::uint32_t height);
VkImageAspectFlags aspectOf(VkFormat format);

// Full-pipeline synchronization2 image layout transition; simple and correct, not minimal.
void transitionImage(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect,
                     VkImageLayout from, VkImageLayout to, std::uint32_t baseMip = 0,
                     std::uint32_t mipCount = VK_REMAINING_MIP_LEVELS,
                     std::uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS);

} // namespace apex::render
