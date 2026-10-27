#pragma once

// Bindless texture heap (plan 0.1, D13): one descriptor set holding a large array of sampled
// images, indexed by slot in shaders. Update-after-bind and partially bound, so slots can be
// written while frames that do not use them are in flight.
//
// Shader side (set number is the caller's choice):
//   layout(set = N, binding = 0) uniform texture2D textures[];   // textures[nonuniformEXT(slot)]
//   layout(set = N, binding = 1) uniform sampler samplers[2];    // 0 = linear/aniso repeat, 1 = nearest repeat
//
// Slot 0 is a magenta/black checkerboard; every unused or released slot also shows it.

#include "assets/vtf.hpp"
#include "core/result.hpp"
#include "render/resources.hpp"
#include "render/upload_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace apex::render {

// The Vulkan format a decoded texture uploads as; none for formats the heap cannot hold (float RGBA).
std::optional<VkFormat> vulkanFormat(assets::TextureFormat format);

using TextureSlot = std::uint32_t;
inline constexpr TextureSlot kCheckerboardSlot = 0;
inline constexpr std::uint32_t kThumbnailTextureSlots = 64;

struct TextureHeapDesc {
    std::uint32_t capacity = 4096;      // Slots, clamped to the device update-after-bind limit.
    VkDeviceSize stagingBytes = 64u << 20;
};

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM; // See formatInfo(): RGBA8 and BC1-BC7 among others.
};

class TextureHeap {
  public:
    static Result<std::unique_ptr<TextureHeap>> create(Context& context, const TextureHeapDesc& desc = {});
    ~TextureHeap();
    TextureHeap(const TextureHeap&) = delete;
    TextureHeap& operator=(const TextureHeap&) = delete;

    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet descriptorSet() const { return set_; }
    std::uint32_t capacity() const { return capacity_; }
    UploadRing& uploadRing() { return ring_; } // Shared with mesh uploads.

    // Reserves one contiguous range for the heap's lifetime. Repeated calls return the same
    // range; normal upload/release never borrow these slots. Fails if no free range remains.
    Result<TextureSlot> reserveThumbnailSlots();
    // Only after prior thumbnail draws using this slot have completed. Replaces in place;
    // the caller owns the range's LRU policy. Single mip, for a future textured thumbnail mode.
    Result<void> uploadThumbnail(TextureSlot slot, const TextureDesc& desc, std::span<const std::byte> pixels);
    void releaseThumbnail(TextureSlot slot);
    std::uint32_t thumbnailSlotsLive() const;
    std::uint32_t maxThumbnailSlotsLive() const { return maxThumbnailSlotsLive_; }

    // Uploads a full mip chain (mips[0] is width x height, each following level halves, rounding
    // down to a minimum of 1). Each level is tightly packed in the texture's format; compressed
    // data is uploaded as-is. The image is sampleable only after flush() (also called implicitly
    // whenever the staging ring fills up).
    Result<TextureSlot> upload(const TextureDesc& desc, std::span<const std::span<const std::byte>> mips);
    // Single-level convenience.
    Result<TextureSlot> upload(const TextureDesc& desc, std::span<const std::byte> pixels);
    // Submits pending uploads and waits.
    Result<void> flush();

    // Points the slot back at the checkerboard and queues its image for destruction. Slot 0 is
    // not releasable. The image may still be in use by in-flight frames, so it is only destroyed
    // by collectGarbage(), which the caller invokes once those frames have completed.
    void release(TextureSlot slot);
    void collectGarbage();
    std::uint32_t liveTextures() const; // Including the checkerboard.

  private:
    TextureHeap() = default;
    Result<TextureSlot> uploadAt(TextureSlot slot, const TextureDesc& desc,
                                 std::span<const std::span<const std::byte>> mips);
    bool thumbnailSlot(TextureSlot slot) const;
    TextureSlot thumbnailFirst_ = 0;
    std::uint32_t maxThumbnailSlotsLive_ = 0;
    void writeDescriptor(TextureSlot slot, VkImageView view);

    Context* context_ = nullptr;
    UploadRing ring_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkSampler samplers_[2]{};
    std::uint32_t capacity_ = 0;
    std::vector<Image> images_;           // Indexed by slot; empty Image when free.
    std::vector<TextureSlot> freeSlots_;
    std::vector<Image> garbage_;
};

} // namespace apex::render
