#pragma once

// Host-visible staging ring with an owned command buffer (plan 0.1, D13). Callers stage bytes,
// then record copies from buffer() into the command buffer returned by commands(). flush()
// submits the batch and waits (uploads are synchronous at batch granularity, which is enough for
// on-demand rendering). When the ring fills up, stage() flushes by itself and starts over; the
// ring grows if a single block is larger than its capacity.

#include "core/result.hpp"
#include "render/resources.hpp"

#include <cstddef>
#include <span>

namespace apex::render {

class UploadRing {
  public:
    static Result<UploadRing> create(Context& context, VkDeviceSize capacity = 64u << 20);

    UploadRing() = default;
    ~UploadRing();
    UploadRing(UploadRing&& other) noexcept;
    UploadRing& operator=(UploadRing&& other) noexcept;
    UploadRing(const UploadRing&) = delete;
    UploadRing& operator=(const UploadRing&) = delete;

    // Copies `data` into the ring and returns its offset in buffer(), aligned to `alignment`.
    // May flush pending commands first, so call commands() AFTER stage() and record the copy
    // before the next stage().
    Result<VkDeviceSize> stage(std::span<const std::byte> data, VkDeviceSize alignment = 16);
    VkBuffer buffer() const { return staging_.handle(); }
    // Command buffer of the batch in progress (begun lazily).
    Result<VkCommandBuffer> commands();
    // Submits recorded work and waits; no-op if nothing is pending.
    Result<void> flush();

  private:
    void release();
    Context* context_ = nullptr;
    Buffer staging_;
    VkDeviceSize capacity_ = 0;
    VkDeviceSize head_ = 0;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    bool recording_ = false;
};

} // namespace apex::render
