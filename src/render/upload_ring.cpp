#include "render/upload_ring.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace apex::render {

Result<UploadRing> UploadRing::create(Context& context, VkDeviceSize capacity) {
    UploadRing ring;
    ring.context_ = &context;
    ring.capacity_ = capacity;
    auto staging = Buffer::create(context, {capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload});
    if (!staging) return std::unexpected(staging.error().withContext("upload ring"));
    ring.staging_ = std::move(*staging);
    VkCommandBufferAllocateInfo ai = vkStruct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    ai.commandPool = context.commandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    APEX_VK_TRY(vkAllocateCommandBuffers(context.device(), &ai, &ring.commands_), "upload ring command buffer");
    return ring;
}

UploadRing::~UploadRing() {
    release();
}

UploadRing::UploadRing(UploadRing&& other) noexcept {
    *this = std::move(other);
}

UploadRing& UploadRing::operator=(UploadRing&& other) noexcept {
    if (this != &other) {
        release();
        context_ = std::exchange(other.context_, nullptr);
        staging_ = std::move(other.staging_);
        capacity_ = std::exchange(other.capacity_, 0);
        head_ = std::exchange(other.head_, 0);
        commands_ = std::exchange(other.commands_, VK_NULL_HANDLE);
        recording_ = std::exchange(other.recording_, false);
    }
    return *this;
}

void UploadRing::release() {
    if (commands_ != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(context_->device(), context_->commandPool(), 1, &commands_);
    }
    commands_ = VK_NULL_HANDLE;
    staging_.reset();
    recording_ = false;
}

Result<VkCommandBuffer> UploadRing::commands() {
    if (!recording_) {
        VkCommandBufferBeginInfo bi = vkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        APEX_VK_TRY(vkBeginCommandBuffer(commands_, &bi), "upload ring begin");
        recording_ = true;
    }
    return commands_;
}

Result<void> UploadRing::flush() {
    if (!recording_) {
        head_ = 0;
        return {};
    }
    recording_ = false;
    APEX_VK_TRY(vkEndCommandBuffer(commands_), "upload ring end");
    auto submitted = context_->submitAndWait(commands_);
    vkResetCommandBuffer(commands_, 0);
    head_ = 0;
    return submitted;
}

Result<VkDeviceSize> UploadRing::stage(std::span<const std::byte> data, VkDeviceSize alignment) {
    const VkDeviceSize size = data.size();
    alignment = std::max<VkDeviceSize>(alignment, 1);
    auto aligned = [&](VkDeviceSize v) { return (v + alignment - 1) / alignment * alignment; };
    if (aligned(head_) + size > capacity_) {
        if (auto flushed = flush(); !flushed) return std::unexpected(flushed.error());
        if (size > capacity_) {
            capacity_ = std::max(size, capacity_ * 2);
            auto bigger = Buffer::create(*context_, {capacity_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload});
            if (!bigger) return std::unexpected(bigger.error().withContext("grow upload ring"));
            staging_ = std::move(*bigger);
        }
    }
    const VkDeviceSize offset = aligned(head_);
    if (size) std::memcpy(static_cast<std::byte*>(staging_.mapped()) + offset, data.data(), size);
    staging_.flush(offset, size);
    head_ = offset + size;
    return offset;
}

} // namespace apex::render
