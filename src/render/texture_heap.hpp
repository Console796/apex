#pragma once

// Bindless texture heap (plan 0.1, D13): one descriptor set holding a large array of sampled
// images, indexed by slot in shaders. Update-after-bind and partially bound, so slots can be
// written while frames that do not use them are in flight.
//
// Shader side (set number is the caller's choice):
//   layout(set = N, binding = 0) uniform texture2D textures[];   // textures[nonuniformEXT(slot)]
//   layout(set = N, binding = 1) uniform sampler samplers[3];    // 0 = linear/aniso repeat, 1 = nearest repeat, 2 = reversed-Z comparison
//   layout(set = N, binding = 2) uniform textureCube cubes[];    // cubes[nonuniformEXT(cubeSlot)]
//
// Slot 0 is a magenta/black checkerboard; every unused or released slot also shows it.
// Cube maps (plan 0.6.5 d) have their own slots and free list. Cube slot 0 is the default cube
// (engine/defaultcubemap once setDefaultCube() succeeds, a mid-grey 1x1 cube until then); every
// unused or released cube slot shows it. Layers are VTF face order, used as Vulkan's
// +X,-X,+Y,-Y,+Z,-Z: shaders look up world (Z-up) directions directly. Provisional (D2).

#include "assets/vtf.hpp"
#include "core/result.hpp"
#include "render/resources.hpp"
#include "render/gpu_material.hpp"
#include "render/upload_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace apex::render {

// The Vulkan format of decoded texture data.
std::optional<VkFormat> vulkanFormat(assets::TextureFormat format);

using TextureSlot = std::uint32_t;
inline constexpr TextureSlot kCheckerboardSlot = 0;
inline constexpr std::uint32_t kThumbnailTextureSlots = 64;
inline constexpr TextureSlot kDefaultCubeSlot = 0;

struct TextureHeapDesc {
    std::uint32_t capacity = 4096;      // Slots, clamped to the device update-after-bind limit.
    VkDeviceSize stagingBytes = 64u << 20;
    std::uint32_t cubeCapacity = 256;   // Cube slots, including the default.
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

    // Borrows an attachment; no upload or layout transition. The owner must keep the image alive
    // and transition it to SHADER_READ_ONLY_OPTIMAL before sampling. Only colour or depth aspects
    // are accepted; the heap owns a dedicated view. Release only after all draws using the slot
    // have completed (the pane's previous-frame fence, or waitIdle in tests/teardown).
    Result<TextureSlot> registerAttachment(const Image& image, VkImageAspectFlags aspect);
    void releaseAttachment(TextureSlot slot);

    // Uploads a full mip chain (mips[0] is width x height, each following level halves, rounding
    // down to a minimum of 1). Each level is tightly packed in the texture's format; compressed
    // data is uploaded as-is. The image is sampleable only after flush() (also called implicitly
    // whenever the staging ring fills up).
    Result<TextureSlot> upload(const TextureDesc& desc, std::span<const std::span<const std::byte>> mips);
    // First frame/face, all mips; float RGBA is packed to RGBA16F without colour decoding.
    Result<TextureSlot> upload(const assets::Texture& texture);
    void setMaterial(TextureSlot slot, GpuMaterial material);
    std::span<const GpuMaterial> materials() const { return materials_; }
    std::uint64_t materialRevision() const { return materialRevision_; }
    // Single-level convenience.
    Result<TextureSlot> upload(const TextureDesc& desc, std::span<const std::byte> pixels);

    // Cube maps: desc is one square face; each mip holds its six faces tightly packed, in layer
    // order. Fails with "cube heap is full" when no cube slot is free.
    Result<TextureSlot> uploadCube(const TextureDesc& desc, std::span<const std::span<const std::byte>> mips);
    // First frame, six faces (a seventh sphere-map face is ignored), all mips. Float RGBA is packed
    // to RGBA16F; 8-bit and BC colour data is gamma-encoded, so it is viewed through the _SRGB
    // format and samples linear (the sRGB curve approximates the base textures' 2.2; provisional).
    Result<TextureSlot> uploadCube(const assets::Texture& texture);
    // Replaces cube slot 0. Waits for the device, since every free cube slot shows slot 0 too.
    Result<void> setDefaultCube(const assets::Texture& texture);
    bool defaultCubeLoaded() const { return defaultCubeLoaded_; }
    // Like release(); cube slot 0 is not releasable.
    void releaseCube(TextureSlot slot);
    std::uint32_t cubeCapacity() const { return cubeCapacity_; }
    std::uint32_t liveCubes() const; // Including the default cube.
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
    Result<Image> createImage(const TextureDesc& desc, std::span<const std::span<const std::byte>> mips, bool cube);
    Result<Image> createCube(const assets::Texture& texture);
    void writeCubeDescriptor(TextureSlot slot, VkImageView view);
    bool thumbnailSlot(TextureSlot slot) const;
    TextureSlot thumbnailFirst_ = 0;
    std::uint32_t maxThumbnailSlotsLive_ = 0;
    void writeDescriptor(TextureSlot slot, VkImageView view);

    std::vector<GpuMaterial> materials_;
    std::uint64_t materialRevision_ = 0;
    Context* context_ = nullptr;
    UploadRing ring_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkSampler samplers_[3]{};
    std::uint32_t capacity_ = 0;
    std::vector<Image> images_;           // Indexed by slot; empty Image when free.
    std::vector<VkImageView> attachments_; // Borrowed images, owned sampled views, indexed by slot.
    std::vector<TextureSlot> freeSlots_;
    std::vector<Image> garbage_;
    std::uint32_t cubeCapacity_ = 0;
    std::vector<Image> cubes_;            // Indexed by cube slot; empty Image when free.
    std::vector<TextureSlot> freeCubes_;
    bool defaultCubeLoaded_ = false;
};

} // namespace apex::render
